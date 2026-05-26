// OpenRipper - src/backends/d3d11/overlay_d3d11.cpp
//
// D2D1.1 text overlay using the D2D1CreateDevice() free function so D2D1
// shares the game's D3D11 adapter — no factory-level device/adapter mismatch.
// Falls back to a window-title flash if D2D1 cannot attach.

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

// ---- D2D1.1 / DWrite state (render thread only) ----------------------------
IDWriteFactory*       g_dwrite     = nullptr;
IDWriteTextFormat*    g_fmt        = nullptr;
ID2D1DeviceContext*   g_d2d_ctx    = nullptr;  // per-swap-chain RT
ID2D1Bitmap1*         g_d2d_bitmap = nullptr;  // wraps the back buffer
ID2D1SolidColorBrush* g_brush_bg   = nullptr;
ID2D1SolidColorBrush* g_brush_text = nullptr;

bool g_init_attempted = false;
bool g_d2d_available  = false;

// ---- Fallback (window-title) ------------------------------------------------
HWND     g_fallback_hwnd   = nullptr;
wchar_t  g_original_title[256]{};
bool     g_fallback_active = false;

// ---- Common ----------------------------------------------------------------
wchar_t       g_overlay_text[128]{};
std::uint32_t g_overlay_frames = 0;

// ----------------------------------------------------------------------------

void release_rt() {
    if (g_brush_text) { g_brush_text->Release(); g_brush_text = nullptr; }
    if (g_brush_bg)   { g_brush_bg->Release();   g_brush_bg   = nullptr; }
    if (g_d2d_bitmap) { g_d2d_bitmap->Release(); g_d2d_bitmap = nullptr; }
    if (g_d2d_ctx)    { g_d2d_ctx->Release();    g_d2d_ctx    = nullptr; }
}

bool create_rt(IDXGISwapChain* sc) {
    // Get the D3D11 device from the swap chain, then QI for IDXGIDevice so
    // D2D1 shares the same adapter (avoids the device/adapter mismatch that
    // causes ID2D1Factory1::CreateDevice to return E_INVALIDARG on some setups).
    ID3D11Device* d3d_dev = nullptr;
    if (FAILED(sc->GetDevice(__uuidof(ID3D11Device),
                             reinterpret_cast<void**>(&d3d_dev))) || !d3d_dev) {
        OR_LOG_WARN("overlay: GetDevice(ID3D11Device) failed");
        return false;
    }

    IDXGIDevice* dxgi_dev = nullptr;
    HRESULT hr = d3d_dev->QueryInterface(__uuidof(IDXGIDevice),
                                         reinterpret_cast<void**>(&dxgi_dev));
    d3d_dev->Release();
    if (FAILED(hr) || !dxgi_dev) {
        OR_LOG_WARN("overlay: QI(IDXGIDevice) failed (hr=0x{:08X})",
                    static_cast<unsigned>(hr));
        return false;
    }

    // D2D1CreateDevice (free function from d2d1_1.h) — creates a D2D1 device
    // on the same GPU as the D3D11 device, bypassing the factory.
    ID2D1Device* d2d_dev = nullptr;
    hr = ::D2D1CreateDevice(dxgi_dev, nullptr, &d2d_dev);
    dxgi_dev->Release();
    if (FAILED(hr)) {
        OR_LOG_WARN("overlay: D2D1CreateDevice failed (hr=0x{:08X})",
                    static_cast<unsigned>(hr));
        return false;
    }

    hr = d2d_dev->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &g_d2d_ctx);
    d2d_dev->Release();
    if (FAILED(hr)) {
        OR_LOG_WARN("overlay: CreateDeviceContext failed (hr=0x{:08X})",
                    static_cast<unsigned>(hr));
        return false;
    }

    IDXGISurface* surf = nullptr;
    if (FAILED(sc->GetBuffer(0, __uuidof(IDXGISurface),
                             reinterpret_cast<void**>(&surf)))) {
        OR_LOG_WARN("overlay: GetBuffer(IDXGISurface) failed");
        release_rt();
        return false;
    }

    D2D1_BITMAP_PROPERTIES1 bmp_props = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_UNKNOWN, D2D1_ALPHA_MODE_PREMULTIPLIED));

    hr = g_d2d_ctx->CreateBitmapFromDxgiSurface(surf, &bmp_props, &g_d2d_bitmap);
    surf->Release();
    if (FAILED(hr)) {
        OR_LOG_WARN("overlay: CreateBitmapFromDxgiSurface failed (hr=0x{:08X})",
                    static_cast<unsigned>(hr));
        release_rt();
        return false;
    }

    g_d2d_ctx->SetTarget(g_d2d_bitmap);
    g_d2d_ctx->CreateSolidColorBrush(D2D1::ColorF(0.f, 0.f, 0.f, 0.65f), &g_brush_bg);
    g_d2d_ctx->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White),   &g_brush_text);
    return (g_brush_bg && g_brush_text);
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
        OR_LOG_WARN("overlay: D2D1 RT init failed — using window-title fallback");
        setup_fallback(sc);
        return;
    }

    g_d2d_available = true;
    OR_LOG_INFO("overlay: D2D1.1 initialised");
}

} // namespace

// ----------------------------------------------------------------------------

void overlay_draw(IDXGISwapChain* sc, std::uint64_t /*frame_id*/) {
    lazy_init(sc);
    if (g_overlay_frames == 0) return;

    if (g_d2d_available) {
        if (!g_d2d_ctx) {
            if (!create_rt(sc)) {
                --g_overlay_frames;
                return;
            }
        }

        g_d2d_ctx->BeginDraw();
        g_d2d_ctx->SetTransform(D2D1::Matrix3x2F::Identity());

        const D2D1_RECT_F bg_rect   = D2D1::RectF(8.f, 8.f, 420.f, 38.f);
        const D2D1_RECT_F text_rect = D2D1::RectF(14.f, 10.f, 416.f, 36.f);

        g_d2d_ctx->FillRectangle(bg_rect, g_brush_bg);
        g_d2d_ctx->DrawText(g_overlay_text,
                            static_cast<UINT32>(::wcslen(g_overlay_text)),
                            g_fmt, text_rect, g_brush_text);

        HRESULT hr = g_d2d_ctx->EndDraw();
        if (FAILED(hr)) {
            release_rt();  // recreated on next frame
        }
    }

    --g_overlay_frames;
    if (g_overlay_frames == 0 && g_fallback_active && g_fallback_hwnd) {
        ::SetWindowTextW(g_fallback_hwnd, g_original_title);
    }
}

void overlay_notify(std::uint64_t frame_id, std::uint32_t draw_count,
                    std::uint32_t frames_to_show) {
    ::swprintf_s(g_overlay_text,
                 L"CAPTURED — frame %06llu  (%u draw%s)",
                 static_cast<unsigned long long>(frame_id),
                 draw_count,
                 draw_count == 1 ? L"" : L"s");
    g_overlay_frames = frames_to_show;

    if (g_fallback_active && g_fallback_hwnd) {
        ::SetWindowTextW(g_fallback_hwnd, g_overlay_text);
    }
}

void overlay_shutdown() {
    if (g_fallback_active && g_fallback_hwnd && g_overlay_frames > 0)
        ::SetWindowTextW(g_fallback_hwnd, g_original_title);

    release_rt();
    if (g_fmt)    { g_fmt->Release();    g_fmt    = nullptr; }
    if (g_dwrite) { g_dwrite->Release(); g_dwrite = nullptr; }

    g_init_attempted  = false;
    g_d2d_available   = false;
    g_fallback_active = false;
    g_overlay_frames  = 0;
}

} // namespace openripper::backends::d3d11
