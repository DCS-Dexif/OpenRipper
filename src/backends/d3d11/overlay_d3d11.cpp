// OpenRipper - src/backends/d3d11/overlay_d3d11.cpp
//
// D2D1.1 text overlay — two rendering modes:
//
//   Direct   (game device has D3D11_CREATE_DEVICE_BGRA_SUPPORT):
//              D2D1 renders straight to the swap chain back buffer.
//
//   Indirect (game device lacks BGRA support — the common case for real games):
//              A small BGRA-capable helper D3D11 device is created on the same
//              adapter. D2D1 renders to a 412x30 helper texture, the pixels are
//              read back to the CPU, then stamped onto the game's back buffer
//              via UpdateSubresource. The round-trip is ~49 KB per frame and
//              only runs while the overlay is visible (~2 s per capture).
//
//   Window-title fallback: used if both D2D1 paths fail (very old hardware,
//              driver bug, etc.).

#include "overlay_d3d11.hpp"

#include "core/logger.hpp"

#include <windows.h>
#include <d3d11.h>
#include <d2d1_1.h>
#include <d2d1_1helper.h>
#include <dwrite.h>
#include <dxgi.h>

#include <cstdio>

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")

namespace openripper::backends::d3d11 {
namespace {

// Overlay rectangle on the back buffer: position (8, 8), size 412x30.
constexpr UINT  kOvX = 8, kOvY = 8, kOvW = 412, kOvH = 30;

// ---- D2D1.1 / DWrite state (render thread only) ----------------------------
IDWriteFactory*       g_dwrite     = nullptr;
IDWriteTextFormat*    g_fmt        = nullptr;
ID2D1DeviceContext*   g_d2d_ctx    = nullptr;
ID2D1Bitmap1*         g_d2d_bitmap = nullptr;
ID2D1SolidColorBrush* g_brush_bg   = nullptr;
ID2D1SolidColorBrush* g_brush_text = nullptr;

bool g_init_attempted = false;
bool g_d2d_available  = false;

// ---- Indirect-mode helper device -------------------------------------------
ID3D11Device*        g_helper_dev     = nullptr;  // BGRA-capable device
ID3D11DeviceContext*  g_helper_ctx     = nullptr;
ID3D11Texture2D*     g_helper_rt      = nullptr;  // D2D1 renders here
ID3D11Texture2D*     g_helper_staging = nullptr;  // CPU-readable readback copy
bool                 g_uses_helper    = false;

// ---- Fallback (window-title) ------------------------------------------------
HWND     g_fallback_hwnd   = nullptr;
wchar_t  g_original_title[256]{};
bool     g_fallback_active = false;

// ---- Common ----------------------------------------------------------------
wchar_t       g_overlay_text[128]{};
std::uint32_t g_overlay_frames = 0;

// ----------------------------------------------------------------------------

void release_rt() {
    if (g_brush_text)     { g_brush_text->Release();     g_brush_text     = nullptr; }
    if (g_brush_bg)       { g_brush_bg->Release();       g_brush_bg       = nullptr; }
    if (g_d2d_bitmap)     { g_d2d_bitmap->Release();     g_d2d_bitmap     = nullptr; }
    if (g_d2d_ctx)        { g_d2d_ctx->Release();        g_d2d_ctx        = nullptr; }
    // Helper textures are re-created in create_rt() if needed; device stays alive.
    if (g_helper_staging) { g_helper_staging->Release(); g_helper_staging = nullptr; }
    if (g_helper_rt)      { g_helper_rt->Release();      g_helper_rt      = nullptr; }
}

// ---- Direct mode: D2D1 on the game's own device ----------------------------
// Returns true on success; leaves g_d2d_ctx / g_d2d_bitmap / brushes live.
bool create_rt_direct(IDXGISwapChain* sc) {
    ID3D11Device* d3d_dev = nullptr;
    if (FAILED(sc->GetDevice(__uuidof(ID3D11Device),
                             reinterpret_cast<void**>(&d3d_dev))) || !d3d_dev)
        return false;

    IDXGIDevice* dxgi_dev = nullptr;
    HRESULT hr = d3d_dev->QueryInterface(__uuidof(IDXGIDevice),
                                         reinterpret_cast<void**>(&dxgi_dev));
    d3d_dev->Release();
    if (FAILED(hr) || !dxgi_dev) return false;

    ID2D1Device* d2d_dev = nullptr;
    hr = ::D2D1CreateDevice(dxgi_dev, nullptr, &d2d_dev);
    dxgi_dev->Release();
    if (FAILED(hr)) {
        OR_LOG_WARN("overlay: D2D1CreateDevice (direct) failed (hr=0x{:08X}) — trying helper device",
                    static_cast<unsigned>(hr));
        return false;
    }

    hr = d2d_dev->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &g_d2d_ctx);
    d2d_dev->Release();
    if (FAILED(hr)) return false;

    IDXGISurface* surf = nullptr;
    if (FAILED(sc->GetBuffer(0, __uuidof(IDXGISurface),
                             reinterpret_cast<void**>(&surf)))) {
        release_rt();
        return false;
    }

    D2D1_BITMAP_PROPERTIES1 bmp_props = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_UNKNOWN, D2D1_ALPHA_MODE_PREMULTIPLIED));

    hr = g_d2d_ctx->CreateBitmapFromDxgiSurface(surf, &bmp_props, &g_d2d_bitmap);
    surf->Release();
    if (FAILED(hr)) { release_rt(); return false; }

    g_d2d_ctx->SetTarget(g_d2d_bitmap);
    g_d2d_ctx->CreateSolidColorBrush(D2D1::ColorF(0.f, 0.f, 0.f, 0.65f), &g_brush_bg);
    g_d2d_ctx->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White),   &g_brush_text);
    return (g_brush_bg && g_brush_text);
}

// ---- Indirect mode: helper BGRA device + CPU copy --------------------------
// Creates g_helper_dev/ctx/rt/staging plus a D2D1 device context that renders
// to g_helper_rt. On success, overlay_draw() will Map the staging texture and
// UpdateSubresource the pixels into the game's back buffer each frame.
bool create_rt_indirect(IDXGISwapChain* sc) {
    // Get the adapter the game is using.
    ID3D11Device* game_dev = nullptr;
    if (FAILED(sc->GetDevice(__uuidof(ID3D11Device),
                             reinterpret_cast<void**>(&game_dev))) || !game_dev)
        return false;

    IDXGIDevice*  game_dxgi = nullptr;
    IDXGIAdapter* adapter   = nullptr;
    HRESULT hr = game_dev->QueryInterface(__uuidof(IDXGIDevice),
                                          reinterpret_cast<void**>(&game_dxgi));
    game_dev->Release();
    if (FAILED(hr) || !game_dxgi) return false;

    hr = game_dxgi->GetAdapter(&adapter);
    game_dxgi->Release();
    if (FAILED(hr) || !adapter) return false;

    // Create our own D3D11 device with BGRA support on the same adapter.
    constexpr D3D_FEATURE_LEVEL kFeatureLevels[] = {
        D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0
    };
    hr = ::D3D11CreateDevice(
        adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        kFeatureLevels, static_cast<UINT>(std::size(kFeatureLevels)),
        D3D11_SDK_VERSION,
        &g_helper_dev, nullptr, &g_helper_ctx);
    adapter->Release();
    if (FAILED(hr)) {
        OR_LOG_WARN("overlay: helper D3D11CreateDevice failed (hr=0x{:08X})",
                    static_cast<unsigned>(hr));
        return false;
    }

    // D2D1 device from our helper device.
    IDXGIDevice* helper_dxgi = nullptr;
    hr = g_helper_dev->QueryInterface(__uuidof(IDXGIDevice),
                                      reinterpret_cast<void**>(&helper_dxgi));
    if (FAILED(hr) || !helper_dxgi) { release_rt(); return false; }

    ID2D1Device* d2d_dev = nullptr;
    hr = ::D2D1CreateDevice(helper_dxgi, nullptr, &d2d_dev);
    helper_dxgi->Release();
    if (FAILED(hr)) {
        OR_LOG_WARN("overlay: D2D1CreateDevice (helper) failed (hr=0x{:08X})",
                    static_cast<unsigned>(hr));
        release_rt();
        return false;
    }

    hr = d2d_dev->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &g_d2d_ctx);
    d2d_dev->Release();
    if (FAILED(hr)) { release_rt(); return false; }

    // Render-target texture on the helper device (D2D1 renders here).
    D3D11_TEXTURE2D_DESC rt_desc{};
    rt_desc.Width            = kOvW;
    rt_desc.Height           = kOvH;
    rt_desc.MipLevels        = 1;
    rt_desc.ArraySize        = 1;
    rt_desc.Format           = DXGI_FORMAT_B8G8R8A8_UNORM;
    rt_desc.SampleDesc.Count = 1;
    rt_desc.Usage            = D3D11_USAGE_DEFAULT;
    rt_desc.BindFlags        = D3D11_BIND_RENDER_TARGET;

    if (FAILED(g_helper_dev->CreateTexture2D(&rt_desc, nullptr, &g_helper_rt))) {
        release_rt(); return false;
    }

    // Staging texture for CPU readback.
    D3D11_TEXTURE2D_DESC stg_desc = rt_desc;
    stg_desc.Usage          = D3D11_USAGE_STAGING;
    stg_desc.BindFlags      = 0;
    stg_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

    if (FAILED(g_helper_dev->CreateTexture2D(&stg_desc, nullptr, &g_helper_staging))) {
        release_rt(); return false;
    }

    // Wrap g_helper_rt as a D2D1 bitmap target.
    IDXGISurface* rt_surf = nullptr;
    if (FAILED(g_helper_rt->QueryInterface(__uuidof(IDXGISurface),
                                           reinterpret_cast<void**>(&rt_surf)))) {
        release_rt(); return false;
    }

    D2D1_BITMAP_PROPERTIES1 bmp_props = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));

    hr = g_d2d_ctx->CreateBitmapFromDxgiSurface(rt_surf, &bmp_props, &g_d2d_bitmap);
    rt_surf->Release();
    if (FAILED(hr)) { release_rt(); return false; }

    g_d2d_ctx->SetTarget(g_d2d_bitmap);

    // Opaque black background: UpdateSubresource doesn't alpha-blend, so we
    // stamp an opaque rectangle. White text on black is readable on any game.
    g_d2d_ctx->CreateSolidColorBrush(D2D1::ColorF(0.f, 0.f, 0.f, 1.f), &g_brush_bg);
    g_d2d_ctx->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White),  &g_brush_text);
    if (!g_brush_bg || !g_brush_text) { release_rt(); return false; }

    g_uses_helper = true;
    return true;
}

bool create_rt(IDXGISwapChain* sc) {
    if (create_rt_direct(sc)) return true;
    return create_rt_indirect(sc);
}

void setup_fallback(IDXGISwapChain* sc) {
    DXGI_SWAP_CHAIN_DESC desc{};
    sc->GetDesc(&desc);
    g_fallback_hwnd = desc.OutputWindow;
    ::GetWindowTextW(g_fallback_hwnd, g_original_title, 256);
    g_fallback_active = true;
}

void lazy_init(IDXGISwapChain* sc) {
    if (g_init_attempted) return;
    g_init_attempted = true;

    HRESULT hr = ::DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
                                       __uuidof(IDWriteFactory),
                                       reinterpret_cast<IUnknown**>(&g_dwrite));
    if (FAILED(hr)) {
        OR_LOG_WARN("overlay: DWriteCreateFactory failed — using window-title fallback");
        setup_fallback(sc);
        return;
    }

    hr = g_dwrite->CreateTextFormat(L"Arial", nullptr,
                                    DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL,
                                    DWRITE_FONT_STRETCH_NORMAL,
                                    18.f, L"", &g_fmt);
    if (FAILED(hr)) {
        OR_LOG_WARN("overlay: CreateTextFormat failed — using window-title fallback");
        g_dwrite->Release(); g_dwrite = nullptr;
        setup_fallback(sc);
        return;
    }

    if (!create_rt(sc)) {
        OR_LOG_WARN("overlay: all D2D1 paths failed — using window-title fallback");
        setup_fallback(sc);
        return;
    }

    g_d2d_available = true;
    if (g_uses_helper)
        OR_LOG_INFO("overlay: D2D1.1 initialised (helper device — game lacks BGRA support)");
    else
        OR_LOG_INFO("overlay: D2D1.1 initialised");
}

} // namespace

// ----------------------------------------------------------------------------

void overlay_draw(IDXGISwapChain* sc, std::uint64_t /*frame_id*/) {
    lazy_init(sc);
    if (g_overlay_frames == 0) return;

    if (g_d2d_available) {
        if (!g_d2d_ctx) {
            // Recover from a prior EndDraw failure (device lost, etc.).
            if (!create_rt(sc)) {
                --g_overlay_frames;
                return;
            }
        }

        if (g_uses_helper) {
            // ---- Indirect mode: render to helper texture, then CPU-copy ----
            // Coordinates are relative to the helper texture (kOvW x kOvH),
            // not to the back buffer.
            const D2D1_RECT_F bg_rect   = D2D1::RectF(0.f, 0.f,
                                                        static_cast<float>(kOvW),
                                                        static_cast<float>(kOvH));
            const D2D1_RECT_F text_rect = D2D1::RectF(6.f, 2.f,
                                                        static_cast<float>(kOvW) - 4.f,
                                                        static_cast<float>(kOvH) - 2.f);

            g_d2d_ctx->BeginDraw();
            g_d2d_ctx->SetTransform(D2D1::Matrix3x2F::Identity());
            g_d2d_ctx->FillRectangle(bg_rect, g_brush_bg);
            g_d2d_ctx->DrawText(g_overlay_text,
                                static_cast<UINT32>(::wcslen(g_overlay_text)),
                                g_fmt, text_rect, g_brush_text);
            HRESULT hr = g_d2d_ctx->EndDraw();
            if (FAILED(hr)) {
                release_rt();
            } else {
                // Read back to CPU via staging, then stamp onto the game's back buffer.
                g_helper_ctx->CopyResource(g_helper_staging, g_helper_rt);

                D3D11_MAPPED_SUBRESOURCE mapped{};
                if (SUCCEEDED(g_helper_ctx->Map(g_helper_staging, 0,
                                                D3D11_MAP_READ, 0, &mapped))) {
                    ID3D11Device*        game_dev = nullptr;
                    ID3D11DeviceContext*  game_ctx = nullptr;
                    ID3D11Texture2D*     bb       = nullptr;

                    if (SUCCEEDED(sc->GetDevice(__uuidof(ID3D11Device),
                                               reinterpret_cast<void**>(&game_dev)))) {
                        game_dev->GetImmediateContext(&game_ctx);
                        if (SUCCEEDED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D),
                                                    reinterpret_cast<void**>(&bb)))) {
                            // Destination box: overlay position on the back buffer.
                            const D3D11_BOX dst_box{ kOvX, kOvY, 0,
                                                     kOvX + kOvW, kOvY + kOvH, 1 };
                            game_ctx->UpdateSubresource(bb, 0, &dst_box,
                                                        mapped.pData,
                                                        mapped.RowPitch, 0);
                            bb->Release();
                        }
                        game_ctx->Release();
                        game_dev->Release();
                    }

                    g_helper_ctx->Unmap(g_helper_staging, 0);
                }
            }

        } else {
            // ---- Direct mode: D2D1 renders straight to the back buffer -----
            const D2D1_RECT_F bg_rect   = D2D1::RectF(8.f, 8.f, 420.f, 38.f);
            const D2D1_RECT_F text_rect = D2D1::RectF(14.f, 10.f, 416.f, 36.f);

            g_d2d_ctx->BeginDraw();
            g_d2d_ctx->SetTransform(D2D1::Matrix3x2F::Identity());
            g_d2d_ctx->FillRectangle(bg_rect, g_brush_bg);
            g_d2d_ctx->DrawText(g_overlay_text,
                                static_cast<UINT32>(::wcslen(g_overlay_text)),
                                g_fmt, text_rect, g_brush_text);
            if (FAILED(g_d2d_ctx->EndDraw()))
                release_rt();
        }
    }

    --g_overlay_frames;
    if (g_overlay_frames == 0 && g_fallback_active && g_fallback_hwnd)
        ::SetWindowTextW(g_fallback_hwnd, g_original_title);
}

void overlay_notify(std::uint64_t frame_id, std::uint32_t draw_count,
                    std::uint32_t frames_to_show) {
    ::swprintf_s(g_overlay_text,
                 L"CAPTURED — frame %06llu  (%u draw%s)",
                 static_cast<unsigned long long>(frame_id),
                 draw_count,
                 draw_count == 1 ? L"" : L"s");
    g_overlay_frames = frames_to_show;

    if (g_fallback_active && g_fallback_hwnd)
        ::SetWindowTextW(g_fallback_hwnd, g_overlay_text);
}

void overlay_shutdown() {
    if (g_fallback_active && g_fallback_hwnd && g_overlay_frames > 0)
        ::SetWindowTextW(g_fallback_hwnd, g_original_title);

    release_rt();
    if (g_fmt)    { g_fmt->Release();    g_fmt    = nullptr; }
    if (g_dwrite) { g_dwrite->Release(); g_dwrite = nullptr; }

    if (g_helper_ctx) { g_helper_ctx->Release(); g_helper_ctx = nullptr; }
    if (g_helper_dev) { g_helper_dev->Release(); g_helper_dev = nullptr; }

    g_init_attempted  = false;
    g_d2d_available   = false;
    g_uses_helper     = false;
    g_fallback_active = false;
    g_overlay_frames  = 0;
}

} // namespace openripper::backends::d3d11
