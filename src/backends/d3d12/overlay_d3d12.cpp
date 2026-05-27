// OpenRipper - src/backends/d3d12/overlay_d3d12.cpp
//
// D2D1.1 overlay via D3D11On12 interop (primary path), with window-title
// fallback.  D3D11On12 wraps the game's D3D12 device and back buffer so the
// existing D2D1 rendering logic applies unchanged.

#include "overlay_d3d12.hpp"
#include "../../core/logger.hpp"

#include <windows.h>
#include <d3d11.h>
#include <d3d11on12.h>
#include <d3d12.h>
#include <d2d1_1.h>
#include <d2d1_1helper.h>
#include <dwrite.h>
#include <dxgi1_4.h>

#include <cstdio>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")

// g_device and g_game_queue are captured by the CreateCommittedResource and
// ExecuteCommandLists hooks; the overlay reads them at lazy-init time.
namespace openripper::backends::d3d12 {
    extern ID3D12Device*        g_device;
    extern ID3D12CommandQueue*  g_game_queue;
} // namespace

namespace openripper::backends::d3d12 {
namespace {

constexpr UINT  kOvX = 8, kOvY = 8, kOvW = 412, kOvH = 30;

// ---- D3D11On12 / D2D1 state (render thread only) --------------------------
ID3D11Device*         g_d3d11dev       = nullptr;
ID3D11DeviceContext*  g_d3d11ctx       = nullptr;
ID3D11On12Device*     g_d3d11on12      = nullptr;
ID2D1Factory1*        g_d2d_factory    = nullptr;
ID2D1Device*          g_d2d_device     = nullptr;
ID2D1DeviceContext*   g_d2d_ctx        = nullptr;
IDWriteFactory*       g_dwrite         = nullptr;
IDWriteTextFormat*    g_fmt            = nullptr;
ID2D1SolidColorBrush* g_brush_bg       = nullptr;
ID2D1SolidColorBrush* g_brush_text     = nullptr;

// Per-frame back-buffer wrapping (recreated every Present):
ID3D11Resource*  g_wrapped_bb     = nullptr;
ID2D1Bitmap1*    g_d2d_bitmap     = nullptr;

bool g_init_attempted = false;
bool g_d3d11on12_ok   = false;

// ---- Fallback state -------------------------------------------------------
HWND    g_fallback_hwnd    = nullptr;
wchar_t g_original_title[256]{};
bool    g_fallback_active  = false;

// ---- Common ---------------------------------------------------------------
wchar_t       g_overlay_text[128]{};
std::uint32_t g_overlay_frames = 0;

// ---------------------------------------------------------------------------

void release_per_frame() {
    if (g_d2d_bitmap)  { g_d2d_bitmap->Release();  g_d2d_bitmap  = nullptr; }
    if (g_wrapped_bb)  { g_wrapped_bb->Release();   g_wrapped_bb  = nullptr; }
}

bool try_init_d3d11on12([[maybe_unused]] IDXGISwapChain* sc) {
    if (!g_device || !g_game_queue) return false;

    // Create D3D11On12 device.
    IUnknown* queues[] = {g_game_queue};
    HRESULT hr = ::D3D11On12CreateDevice(
        g_device, 0, nullptr, 0,
        queues, 1, 0,
        &g_d3d11dev, &g_d3d11ctx, nullptr);
    if (FAILED(hr)) {
        OR_LOG_WARN("overlay d3d12: D3D11On12CreateDevice failed (hr=0x{:08X})", static_cast<unsigned>(hr));
        return false;
    }
    hr = g_d3d11dev->QueryInterface(IID_PPV_ARGS(&g_d3d11on12));
    if (FAILED(hr)) { g_d3d11dev->Release(); g_d3d11dev = nullptr; return false; }

    // D2D1 factory.
    D2D1_FACTORY_OPTIONS opts{D2D1_DEBUG_LEVEL_NONE};
    hr = ::D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, opts, &g_d2d_factory);
    if (FAILED(hr)) { OR_LOG_WARN("overlay d3d12: D2D1CreateFactory failed"); goto fail; }

    {
        IDXGIDevice* dxgi_dev = nullptr;
        if (FAILED(g_d3d11dev->QueryInterface(IID_PPV_ARGS(&dxgi_dev)))) goto fail;
        hr = g_d2d_factory->CreateDevice(dxgi_dev, &g_d2d_device);
        dxgi_dev->Release();
        if (FAILED(hr)) goto fail;
    }
    if (FAILED(g_d2d_device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &g_d2d_ctx)))
        goto fail;

    // DWrite.
    if (FAILED(::DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
               __uuidof(IDWriteFactory),
               reinterpret_cast<IUnknown**>(&g_dwrite))))
        goto fail;
    if (FAILED(g_dwrite->CreateTextFormat(L"Arial", nullptr,
               DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL,
               DWRITE_FONT_STRETCH_NORMAL, 18.0f, L"en-us", &g_fmt)))
        goto fail;

    OR_LOG_INFO("overlay d3d12: D3D11On12 + D2D1 overlay ready");
    return true;

fail:
    if (g_fmt)        { g_fmt->Release();        g_fmt        = nullptr; }
    if (g_dwrite)     { g_dwrite->Release();     g_dwrite     = nullptr; }
    if (g_d2d_ctx)    { g_d2d_ctx->Release();    g_d2d_ctx    = nullptr; }
    if (g_d2d_device) { g_d2d_device->Release(); g_d2d_device = nullptr; }
    if (g_d2d_factory){ g_d2d_factory->Release();g_d2d_factory= nullptr; }
    if (g_d3d11on12)  { g_d3d11on12->Release();  g_d3d11on12  = nullptr; }
    if (g_d3d11ctx)   { g_d3d11ctx->Release();   g_d3d11ctx   = nullptr; }
    if (g_d3d11dev)   { g_d3d11dev->Release();   g_d3d11dev   = nullptr; }
    return false;
}

bool wrap_back_buffer(IDXGISwapChain* sc) {
    release_per_frame();

    // Get current back buffer index.
    IDXGISwapChain3* sc3 = nullptr;
    UINT bb_idx = 0;
    if (SUCCEEDED(sc->QueryInterface(IID_PPV_ARGS(&sc3)))) {
        bb_idx = sc3->GetCurrentBackBufferIndex();
        sc3->Release();
    }

    ID3D12Resource* bb12 = nullptr;
    if (FAILED(sc->GetBuffer(bb_idx, IID_PPV_ARGS(&bb12)))) return false;

    D3D11_RESOURCE_FLAGS d3d11_flags{D3D11_BIND_RENDER_TARGET};
    HRESULT hr = g_d3d11on12->CreateWrappedResource(
        bb12,
        &d3d11_flags,
        D3D12_RESOURCE_STATE_RENDER_TARGET,
        D3D12_RESOURCE_STATE_PRESENT,
        IID_PPV_ARGS(&g_wrapped_bb));
    bb12->Release();
    if (FAILED(hr)) return false;

    IDXGISurface* surf = nullptr;
    if (FAILED(g_wrapped_bb->QueryInterface(IID_PPV_ARGS(&surf)))) return false;

    D3D12_RESOURCE_DESC bb_desc = {};
    {
        ID3D12Resource* bb12b = nullptr;
        sc->GetBuffer(bb_idx, IID_PPV_ARGS(&bb12b));
        if (bb12b) { bb_desc = bb12b->GetDesc(); bb12b->Release(); }
    }

    D2D1_BITMAP_PROPERTIES1 bmp_props = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_UNKNOWN, D2D1_ALPHA_MODE_PREMULTIPLIED));
    HRESULT bhr = g_d2d_ctx->CreateBitmapFromDxgiSurface(surf, &bmp_props, &g_d2d_bitmap);
    surf->Release();

    if (FAILED(bhr)) {
        OR_LOG_WARN("overlay d3d12: CreateBitmapFromDxgiSurface failed (hr=0x{:08X})", static_cast<unsigned>(bhr));
        return false;
    }

    // Brushes (created once and reused via reconfiguration).
    if (!g_brush_bg) {
        g_d2d_ctx->CreateSolidColorBrush(D2D1::ColorF(0.f, 0.f, 0.f, 0.75f), &g_brush_bg);
        g_d2d_ctx->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White),   &g_brush_text);
    }
    return true;
}

void draw_d2d1_text(IDXGISwapChain* sc) {
    if (!wrap_back_buffer(sc) || !g_d2d_bitmap) return;

    g_d3d11on12->AcquireWrappedResources(&g_wrapped_bb, 1);
    g_d2d_ctx->SetTarget(g_d2d_bitmap);
    g_d2d_ctx->BeginDraw();

    const auto rect = D2D1::RectF(
        static_cast<float>(kOvX), static_cast<float>(kOvY),
        static_cast<float>(kOvX + kOvW), static_cast<float>(kOvY + kOvH));
    g_d2d_ctx->FillRectangle(rect, g_brush_bg);
    g_d2d_ctx->DrawText(g_overlay_text, static_cast<UINT32>(::wcslen(g_overlay_text)),
                        g_fmt, rect, g_brush_text);

    HRESULT hr = g_d2d_ctx->EndDraw();
    if (FAILED(hr)) {
        OR_LOG_WARN("overlay d3d12: EndDraw failed (hr=0x{:08X})", static_cast<unsigned>(hr));
        release_per_frame();
    }

    g_d3d11on12->ReleaseWrappedResources(&g_wrapped_bb, 1);
    g_d3d11ctx->Flush();
}

void set_fallback_title(IDXGISwapChain* sc, std::uint64_t frame_id) {
    DXGI_SWAP_CHAIN_DESC desc{};
    sc->GetDesc(&desc);
    g_fallback_hwnd = desc.OutputWindow;
    if (g_fallback_hwnd) {
        ::GetWindowTextW(g_fallback_hwnd, g_original_title, 256);
        wchar_t buf[256];
        ::swprintf_s(buf, L"[CAPTURED frame %llu] %ls",
                     static_cast<unsigned long long>(frame_id), g_original_title);
        ::SetWindowTextW(g_fallback_hwnd, buf);
        g_fallback_active = true;
    }
}

} // namespace

void overlay_draw(IDXGISwapChain* sc, std::uint64_t frame_id) {
    if (!g_init_attempted) {
        g_init_attempted = true;
        g_d3d11on12_ok   = try_init_d3d11on12(sc);
    }

    if (g_overlay_frames == 0) return;
    --g_overlay_frames;

    if (g_d3d11on12_ok) {
        draw_d2d1_text(sc);
    } else if (!g_fallback_active) {
        set_fallback_title(sc, frame_id);
    }

    if (g_overlay_frames == 0 && g_fallback_active && g_fallback_hwnd) {
        ::SetWindowTextW(g_fallback_hwnd, g_original_title);
        g_fallback_active = false;
    }
}

void overlay_notify(std::uint64_t frame_id, std::uint32_t draw_count,
                    std::uint32_t frames_to_show)
{
    ::swprintf_s(g_overlay_text,
                 L"CAPTURED — frame %llu (%u draws)",
                 static_cast<unsigned long long>(frame_id),
                 draw_count);
    g_overlay_frames = frames_to_show;
    OR_LOG_INFO("overlay d3d12: armed ({} draws)", draw_count);
}

void overlay_shutdown() {
    release_per_frame();
    if (g_brush_bg)    { g_brush_bg->Release();    g_brush_bg    = nullptr; }
    if (g_brush_text)  { g_brush_text->Release();  g_brush_text  = nullptr; }
    if (g_fmt)         { g_fmt->Release();          g_fmt         = nullptr; }
    if (g_dwrite)      { g_dwrite->Release();       g_dwrite      = nullptr; }
    if (g_d2d_ctx)     { g_d2d_ctx->Release();      g_d2d_ctx     = nullptr; }
    if (g_d2d_device)  { g_d2d_device->Release();   g_d2d_device  = nullptr; }
    if (g_d2d_factory) { g_d2d_factory->Release();  g_d2d_factory = nullptr; }
    if (g_d3d11on12)   { g_d3d11on12->Release();    g_d3d11on12   = nullptr; }
    if (g_d3d11ctx)    { g_d3d11ctx->Release();      g_d3d11ctx    = nullptr; }
    if (g_d3d11dev)    { g_d3d11dev->Release();      g_d3d11dev    = nullptr; }

    if (g_fallback_active && g_fallback_hwnd) {
        ::SetWindowTextW(g_fallback_hwnd, g_original_title);
        g_fallback_active = false;
    }
    g_init_attempted = false;
    g_d3d11on12_ok   = false;
}

} // namespace openripper::backends::d3d12
