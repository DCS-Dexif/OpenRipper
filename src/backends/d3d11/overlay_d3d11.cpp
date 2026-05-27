// OpenRipper - src/backends/d3d11/overlay_d3d11.cpp
//
// D3D11 text overlay — robust draw-call compositing approach.
//
// Text is rendered by D2D1 on a small helper BGRA device (412x30 px).
// The pixels are uploaded to a B8G8R8A8 texture on the game's own device,
// then composited onto the back buffer via a fullscreen triangle + scissor rect
// using the game's D3D11 device context.  The GPU handles format conversion,
// so this works with any back buffer format (RGBA8, BGRA8, R10G10B10A2,
// float16) and any swap chain mode including MSAA and FLIP_DISCARD.
//
// Pipeline state is saved and fully restored around the overlay draw, so the
// game's next frame is unaffected.
//
// Falls back to a window-title flash if D3DCompile cannot be loaded or any
// pipeline object creation fails.

#include "overlay_d3d11.hpp"

#include "core/logger.hpp"

#include <windows.h>
#include <d3d11.h>
#include <d2d1_1.h>
#include <d2d1_1helper.h>
#include <dwrite.h>
#include <dxgi.h>
#include <d3dcompiler.h>

#include <cstdio>

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")

namespace openripper::backends::d3d11 {
namespace {

// Overlay rectangle on the back buffer: position (8, 8), size 412x30.
constexpr UINT kOvX = 8, kOvY = 8, kOvW = 412, kOvH = 30;

// ---- HLSL shaders (compiled once at init) -----------------------------------

// Fullscreen triangle from SV_VertexID; no vertex buffer required.
static const char* kVsSrc =
    "float4 main(uint vid : SV_VertexID) : SV_Position {"
    "  float2 uv = float2((vid << 1) & 2, vid & 2);"
    "  return float4(uv.x*2.0-1.0, -uv.y*2.0+1.0, 0.0, 1.0);"
    "}";

// Sample the overlay texture using pixel position to compute [0,1] UVs.
static const char* kPsSrc =
    "Texture2D t : register(t0);"
    "SamplerState s : register(s0);"
    "float4 main(float4 pos : SV_Position) : SV_Target {"
    "  float2 uv = (pos.xy - float2(8.0,8.0)) / float2(412.0,30.0);"
    "  return t.Sample(s, uv);"
    "}";

// ---- D3DCompile loaded dynamically ------------------------------------------
using PFN_D3DCompile = HRESULT(WINAPI*)(
    LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*,
    LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);

static HMODULE       g_d3dcompiler_dll = nullptr;
static PFN_D3DCompile g_d3dcompile     = nullptr;

static bool load_d3dcompiler() {
    if (g_d3dcompile) return true;
    g_d3dcompiler_dll = ::LoadLibraryW(L"d3dcompiler_47.dll");
    if (!g_d3dcompiler_dll) {
        OR_LOG_WARN("overlay: LoadLibraryW(d3dcompiler_47.dll) failed — using title fallback");
        return false;
    }
    g_d3dcompile = reinterpret_cast<PFN_D3DCompile>(
        ::GetProcAddress(g_d3dcompiler_dll, "D3DCompile"));
    if (!g_d3dcompile) {
        OR_LOG_WARN("overlay: D3DCompile not found in d3dcompiler_47.dll — using title fallback");
        ::FreeLibrary(g_d3dcompiler_dll);
        g_d3dcompiler_dll = nullptr;
        return false;
    }
    return true;
}

// ---- D2D1 / DWrite state (helper device — text rendering only) --------------
IDWriteFactory*      g_dwrite          = nullptr;
IDWriteTextFormat*   g_fmt             = nullptr;
ID3D11Device*        g_helper_dev      = nullptr;
ID3D11DeviceContext* g_helper_ctx      = nullptr;
ID2D1DeviceContext*  g_d2d_ctx         = nullptr;
ID2D1Bitmap1*        g_d2d_bitmap      = nullptr;  // render target on helper RT
ID2D1SolidColorBrush* g_brush_bg       = nullptr;
ID2D1SolidColorBrush* g_brush_text     = nullptr;
ID3D11Texture2D*     g_helper_rt       = nullptr;  // D2D1 renders here
ID3D11Texture2D*     g_helper_staging  = nullptr;  // CPU readback copy

// ---- Game-device overlay pipeline -------------------------------------------
ID3D11VertexShader*       g_vs           = nullptr;
ID3D11PixelShader*        g_ps           = nullptr;
ID3D11BlendState*         g_blend        = nullptr;
ID3D11SamplerState*       g_sampler      = nullptr;
ID3D11RasterizerState*    g_rasterizer   = nullptr;
ID3D11DepthStencilState*  g_dss          = nullptr;
ID3D11Texture2D*          g_overlay_tex  = nullptr;  // B8G8R8A8 412x30 on game device
ID3D11ShaderResourceView* g_overlay_srv  = nullptr;

// ---- Init flags -------------------------------------------------------------
bool g_init_attempted = false;
bool g_text_ok        = false;   // D2D1 text path is ready
bool g_composite_ok   = false;   // draw-call compositing path is ready

// ---- Fallback (window-title) ------------------------------------------------
HWND    g_fallback_hwnd   = nullptr;
wchar_t g_original_title[256]{};
bool    g_fallback_active = false;

// ---- Common -----------------------------------------------------------------
wchar_t       g_overlay_text[128]{};
std::uint32_t g_overlay_frames = 0;

// ----------------------------------------------------------------------------

// Pipeline state snapshot for save/restore around the overlay draw.
struct SavedState {
    // RS
    ID3D11RasterizerState* rs_state;
    UINT                   num_vp;
    D3D11_VIEWPORT         viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
    UINT                   num_sc;
    D3D11_RECT             scissors[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
    // OM
    ID3D11RenderTargetView* rtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT];
    ID3D11DepthStencilView* dsv;
    ID3D11BlendState*       blend;
    FLOAT                   blend_factor[4];
    UINT                    blend_mask;
    ID3D11DepthStencilState* dss;
    UINT                    stencil_ref;
    // Shaders
    ID3D11VertexShader*   vs;
    ID3D11PixelShader*    ps;
    ID3D11GeometryShader* gs;
    ID3D11HullShader*     hs;
    ID3D11DomainShader*   ds;
    // IA
    ID3D11InputLayout*         il;
    D3D11_PRIMITIVE_TOPOLOGY   topology;
    ID3D11Buffer*              vb;
    UINT                       vb_stride;
    UINT                       vb_offset;
    ID3D11Buffer*              ib;
    DXGI_FORMAT                ib_fmt;
    UINT                       ib_off;
    // PS resources (only slot 0)
    ID3D11ShaderResourceView* ps_srv;
    ID3D11SamplerState*       ps_sampler;
};

static void save_state(ID3D11DeviceContext* ctx, SavedState& s) {
    ctx->RSGetState(&s.rs_state);
    s.num_vp = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    ctx->RSGetViewports(&s.num_vp, s.viewports);
    s.num_sc = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    ctx->RSGetScissorRects(&s.num_sc, s.scissors);
    ctx->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, s.rtvs, &s.dsv);
    ctx->OMGetBlendState(&s.blend, s.blend_factor, &s.blend_mask);
    ctx->OMGetDepthStencilState(&s.dss, &s.stencil_ref);
    ctx->VSGetShader(&s.vs, nullptr, nullptr);
    ctx->PSGetShader(&s.ps, nullptr, nullptr);
    ctx->GSGetShader(&s.gs, nullptr, nullptr);
    ctx->HSGetShader(&s.hs, nullptr, nullptr);
    ctx->DSGetShader(&s.ds, nullptr, nullptr);
    ctx->IAGetInputLayout(&s.il);
    ctx->IAGetPrimitiveTopology(&s.topology);
    ctx->IAGetVertexBuffers(0, 1, &s.vb, &s.vb_stride, &s.vb_offset);
    ctx->IAGetIndexBuffer(&s.ib, &s.ib_fmt, &s.ib_off);
    ctx->PSGetShaderResources(0, 1, &s.ps_srv);
    ctx->PSGetSamplers(0, 1, &s.ps_sampler);
}

static void restore_state(ID3D11DeviceContext* ctx, SavedState& s) {
    ctx->RSSetState(s.rs_state);
    ctx->RSSetViewports(s.num_vp, s.viewports);
    ctx->RSSetScissorRects(s.num_sc, s.scissors);
    ctx->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, s.rtvs, s.dsv);
    ctx->OMSetBlendState(s.blend, s.blend_factor, s.blend_mask);
    ctx->OMSetDepthStencilState(s.dss, s.stencil_ref);
    ctx->VSSetShader(s.vs, nullptr, 0);
    ctx->PSSetShader(s.ps, nullptr, 0);
    ctx->GSSetShader(s.gs, nullptr, 0);
    ctx->HSSetShader(s.hs, nullptr, 0);
    ctx->DSSetShader(s.ds, nullptr, 0);
    ctx->IASetInputLayout(s.il);
    ctx->IASetPrimitiveTopology(s.topology);
    ctx->IASetVertexBuffers(0, 1, &s.vb, &s.vb_stride, &s.vb_offset);
    ctx->IASetIndexBuffer(s.ib, s.ib_fmt, s.ib_off);
    ctx->PSSetShaderResources(0, 1, &s.ps_srv);
    ctx->PSSetSamplers(0, 1, &s.ps_sampler);

    // Release all saved COM refs
    if (s.rs_state)  s.rs_state->Release();
    for (auto* rtv : s.rtvs) if (rtv) rtv->Release();
    if (s.dsv)       s.dsv->Release();
    if (s.blend)     s.blend->Release();
    if (s.dss)       s.dss->Release();
    if (s.vs)        s.vs->Release();
    if (s.ps)        s.ps->Release();
    if (s.gs)        s.gs->Release();
    if (s.hs)        s.hs->Release();
    if (s.ds)        s.ds->Release();
    if (s.il)        s.il->Release();
    if (s.vb)        s.vb->Release();
    if (s.ib)        s.ib->Release();
    if (s.ps_srv)    s.ps_srv->Release();
    if (s.ps_sampler) s.ps_sampler->Release();
}

// ----------------------------------------------------------------------------

static void setup_fallback(IDXGISwapChain* sc) {
    DXGI_SWAP_CHAIN_DESC desc{};
    sc->GetDesc(&desc);
    g_fallback_hwnd = desc.OutputWindow;
    ::GetWindowTextW(g_fallback_hwnd, g_original_title, 256);
    g_fallback_active = true;
}

// Initialise the D2D1 text-rendering half (helper device + D2D1 context).
// Returns true if g_d2d_ctx / g_d2d_bitmap / brushes are ready.
static bool init_text_path(IDXGISwapChain* sc) {
    // --- DWrite factory + text format ---
    if (FAILED(::DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
                                     __uuidof(IDWriteFactory),
                                     reinterpret_cast<IUnknown**>(&g_dwrite)))) {
        OR_LOG_WARN("overlay: DWriteCreateFactory failed");
        return false;
    }
    if (FAILED(g_dwrite->CreateTextFormat(L"Arial", nullptr,
                                          DWRITE_FONT_WEIGHT_BOLD,
                                          DWRITE_FONT_STYLE_NORMAL,
                                          DWRITE_FONT_STRETCH_NORMAL,
                                          18.f, L"", &g_fmt))) {
        OR_LOG_WARN("overlay: CreateTextFormat failed");
        return false;
    }

    // --- Helper BGRA device ---
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

    constexpr D3D_FEATURE_LEVEL kFL[] = {
        D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0
    };
    hr = ::D3D11CreateDevice(
        adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        kFL, static_cast<UINT>(std::size(kFL)),
        D3D11_SDK_VERSION,
        &g_helper_dev, nullptr, &g_helper_ctx);
    adapter->Release();
    if (FAILED(hr)) {
        OR_LOG_WARN("overlay: helper D3D11CreateDevice failed (hr=0x{:08X})",
                    static_cast<unsigned>(hr));
        return false;
    }

    // --- D2D1 device context from helper device ---
    IDXGIDevice* helper_dxgi = nullptr;
    hr = g_helper_dev->QueryInterface(__uuidof(IDXGIDevice),
                                      reinterpret_cast<void**>(&helper_dxgi));
    if (FAILED(hr)) return false;

    ID2D1Device* d2d_dev = nullptr;
    hr = ::D2D1CreateDevice(helper_dxgi, nullptr, &d2d_dev);
    helper_dxgi->Release();
    if (FAILED(hr)) {
        OR_LOG_WARN("overlay: D2D1CreateDevice (helper) failed (hr=0x{:08X})",
                    static_cast<unsigned>(hr));
        return false;
    }
    hr = d2d_dev->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &g_d2d_ctx);
    d2d_dev->Release();
    if (FAILED(hr)) return false;

    // --- Helper RT (D2D1 renders here) and staging (CPU readback) ---
    D3D11_TEXTURE2D_DESC rt_desc{};
    rt_desc.Width            = kOvW;
    rt_desc.Height           = kOvH;
    rt_desc.MipLevels        = 1;
    rt_desc.ArraySize        = 1;
    rt_desc.Format           = DXGI_FORMAT_B8G8R8A8_UNORM;
    rt_desc.SampleDesc.Count = 1;
    rt_desc.Usage            = D3D11_USAGE_DEFAULT;
    rt_desc.BindFlags        = D3D11_BIND_RENDER_TARGET;
    if (FAILED(g_helper_dev->CreateTexture2D(&rt_desc, nullptr, &g_helper_rt)))
        return false;

    D3D11_TEXTURE2D_DESC stg_desc = rt_desc;
    stg_desc.Usage          = D3D11_USAGE_STAGING;
    stg_desc.BindFlags      = 0;
    stg_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(g_helper_dev->CreateTexture2D(&stg_desc, nullptr, &g_helper_staging)))
        return false;

    // Wrap the helper RT as a D2D1 render target.
    IDXGISurface* rt_surf = nullptr;
    if (FAILED(g_helper_rt->QueryInterface(__uuidof(IDXGISurface),
                                           reinterpret_cast<void**>(&rt_surf))))
        return false;

    D2D1_BITMAP_PROPERTIES1 bmp_props = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    hr = g_d2d_ctx->CreateBitmapFromDxgiSurface(rt_surf, &bmp_props, &g_d2d_bitmap);
    rt_surf->Release();
    if (FAILED(hr)) return false;

    g_d2d_ctx->SetTarget(g_d2d_bitmap);
    g_d2d_ctx->CreateSolidColorBrush(D2D1::ColorF(0.f, 0.f, 0.f, 0.85f), &g_brush_bg);
    g_d2d_ctx->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White),  &g_brush_text);
    return (g_brush_bg && g_brush_text);
}

// Initialise the draw-call compositing half on the game's device.
// Compiles shaders via D3DCompile (loaded dynamically).
// Returns true if g_vs/g_ps/pipeline objects/g_overlay_tex/g_overlay_srv are ready.
static bool init_composite_path(IDXGISwapChain* sc) {
    if (!load_d3dcompiler()) return false;

    ID3D11Device* game_dev = nullptr;
    if (FAILED(sc->GetDevice(__uuidof(ID3D11Device),
                             reinterpret_cast<void**>(&game_dev))) || !game_dev)
        return false;

    // Compile VS
    ID3DBlob* vs_blob  = nullptr;
    ID3DBlob* err_blob = nullptr;
    HRESULT hr = g_d3dcompile(kVsSrc, ::strlen(kVsSrc), "overlay_vs", nullptr, nullptr,
                               "main", "vs_4_0", 0, 0, &vs_blob, &err_blob);
    if (FAILED(hr)) {
        OR_LOG_WARN("overlay: VS compile failed (hr=0x{:08X}) {}",
                    static_cast<unsigned>(hr),
                    err_blob ? static_cast<const char*>(err_blob->GetBufferPointer()) : "");
        if (err_blob) err_blob->Release();
        game_dev->Release();
        return false;
    }
    if (err_blob) err_blob->Release();

    // Compile PS
    ID3DBlob* ps_blob = nullptr;
    hr = g_d3dcompile(kPsSrc, ::strlen(kPsSrc), "overlay_ps", nullptr, nullptr,
                      "main", "ps_4_0", 0, 0, &ps_blob, &err_blob);
    if (FAILED(hr)) {
        OR_LOG_WARN("overlay: PS compile failed (hr=0x{:08X}) {}",
                    static_cast<unsigned>(hr),
                    err_blob ? static_cast<const char*>(err_blob->GetBufferPointer()) : "");
        if (err_blob) err_blob->Release();
        vs_blob->Release();
        game_dev->Release();
        return false;
    }
    if (err_blob) err_blob->Release();

    hr = game_dev->CreateVertexShader(vs_blob->GetBufferPointer(),
                                      vs_blob->GetBufferSize(), nullptr, &g_vs);
    vs_blob->Release();
    if (FAILED(hr)) { ps_blob->Release(); game_dev->Release(); return false; }

    hr = game_dev->CreatePixelShader(ps_blob->GetBufferPointer(),
                                     ps_blob->GetBufferSize(), nullptr, &g_ps);
    ps_blob->Release();
    if (FAILED(hr)) { game_dev->Release(); return false; }

    // Blend: SrcAlpha / InvSrcAlpha on RT0, no color write mask restriction
    D3D11_BLEND_DESC blend_desc{};
    blend_desc.RenderTarget[0].BlendEnable           = TRUE;
    blend_desc.RenderTarget[0].SrcBlend              = D3D11_BLEND_SRC_ALPHA;
    blend_desc.RenderTarget[0].DestBlend             = D3D11_BLEND_INV_SRC_ALPHA;
    blend_desc.RenderTarget[0].BlendOp               = D3D11_BLEND_OP_ADD;
    blend_desc.RenderTarget[0].SrcBlendAlpha         = D3D11_BLEND_ONE;
    blend_desc.RenderTarget[0].DestBlendAlpha        = D3D11_BLEND_ZERO;
    blend_desc.RenderTarget[0].BlendOpAlpha          = D3D11_BLEND_OP_ADD;
    blend_desc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(game_dev->CreateBlendState(&blend_desc, &g_blend))) {
        game_dev->Release(); return false;
    }

    // Sampler: point filter, clamp
    D3D11_SAMPLER_DESC samp_desc{};
    samp_desc.Filter         = D3D11_FILTER_MIN_MAG_MIP_POINT;
    samp_desc.AddressU       = D3D11_TEXTURE_ADDRESS_CLAMP;
    samp_desc.AddressV       = D3D11_TEXTURE_ADDRESS_CLAMP;
    samp_desc.AddressW       = D3D11_TEXTURE_ADDRESS_CLAMP;
    samp_desc.MaxAnisotropy  = 1;
    samp_desc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    samp_desc.MaxLOD         = D3D11_FLOAT32_MAX;
    if (FAILED(game_dev->CreateSamplerState(&samp_desc, &g_sampler))) {
        game_dev->Release(); return false;
    }

    // Rasterizer: no cull, scissor enabled, fill solid
    D3D11_RASTERIZER_DESC rast_desc{};
    rast_desc.FillMode        = D3D11_FILL_SOLID;
    rast_desc.CullMode        = D3D11_CULL_NONE;
    rast_desc.ScissorEnable   = TRUE;
    rast_desc.DepthClipEnable = TRUE;
    if (FAILED(game_dev->CreateRasterizerState(&rast_desc, &g_rasterizer))) {
        game_dev->Release(); return false;
    }

    // Depth-stencil: fully disabled
    D3D11_DEPTH_STENCIL_DESC dss_desc{};
    dss_desc.DepthEnable   = FALSE;
    dss_desc.StencilEnable = FALSE;
    if (FAILED(game_dev->CreateDepthStencilState(&dss_desc, &g_dss))) {
        game_dev->Release(); return false;
    }

    // Overlay texture: 412x30 B8G8R8A8, DEFAULT usage, shader resource.
    D3D11_TEXTURE2D_DESC tex_desc{};
    tex_desc.Width            = kOvW;
    tex_desc.Height           = kOvH;
    tex_desc.MipLevels        = 1;
    tex_desc.ArraySize        = 1;
    tex_desc.Format           = DXGI_FORMAT_B8G8R8A8_UNORM;
    tex_desc.SampleDesc.Count = 1;
    tex_desc.Usage            = D3D11_USAGE_DEFAULT;
    tex_desc.BindFlags        = D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(game_dev->CreateTexture2D(&tex_desc, nullptr, &g_overlay_tex))) {
        game_dev->Release(); return false;
    }
    if (FAILED(game_dev->CreateShaderResourceView(g_overlay_tex, nullptr, &g_overlay_srv))) {
        game_dev->Release(); return false;
    }

    game_dev->Release();
    return true;
}

static void lazy_init(IDXGISwapChain* sc) {
    if (g_init_attempted) return;
    g_init_attempted = true;

    g_text_ok      = init_text_path(sc);
    g_composite_ok = g_text_ok && init_composite_path(sc);

    if (g_composite_ok) {
        OR_LOG_INFO("overlay: draw-call compositing path ready");
    } else if (g_text_ok) {
        OR_LOG_WARN("overlay: composite init failed; text path ready but will use title fallback");
        setup_fallback(sc);
    } else {
        OR_LOG_WARN("overlay: all paths failed — using window-title fallback");
        setup_fallback(sc);
    }
}

static void release_pipeline() {
    if (g_overlay_srv) { g_overlay_srv->Release(); g_overlay_srv = nullptr; }
    if (g_overlay_tex) { g_overlay_tex->Release(); g_overlay_tex = nullptr; }
    if (g_dss)         { g_dss->Release();         g_dss         = nullptr; }
    if (g_rasterizer)  { g_rasterizer->Release();  g_rasterizer  = nullptr; }
    if (g_sampler)     { g_sampler->Release();      g_sampler     = nullptr; }
    if (g_blend)       { g_blend->Release();        g_blend       = nullptr; }
    if (g_ps)          { g_ps->Release();           g_ps          = nullptr; }
    if (g_vs)          { g_vs->Release();           g_vs          = nullptr; }
}

static void release_text() {
    if (g_brush_text)    { g_brush_text->Release();    g_brush_text    = nullptr; }
    if (g_brush_bg)      { g_brush_bg->Release();      g_brush_bg      = nullptr; }
    if (g_d2d_bitmap)    { g_d2d_bitmap->Release();    g_d2d_bitmap    = nullptr; }
    if (g_d2d_ctx)       { g_d2d_ctx->Release();       g_d2d_ctx       = nullptr; }
    if (g_helper_staging){ g_helper_staging->Release();g_helper_staging= nullptr; }
    if (g_helper_rt)     { g_helper_rt->Release();     g_helper_rt     = nullptr; }
    if (g_helper_ctx)    { g_helper_ctx->Release();    g_helper_ctx    = nullptr; }
    if (g_helper_dev)    { g_helper_dev->Release();    g_helper_dev    = nullptr; }
    if (g_fmt)           { g_fmt->Release();           g_fmt           = nullptr; }
    if (g_dwrite)        { g_dwrite->Release();        g_dwrite        = nullptr; }
}

} // namespace

// ----------------------------------------------------------------------------

void overlay_draw(IDXGISwapChain* sc, std::uint64_t /*frame_id*/) {
    lazy_init(sc);
    if (g_overlay_frames == 0) return;

    if (g_text_ok && g_composite_ok) {
        // --- 1. Render text to helper D2D1 texture ---
        const D2D1_RECT_F bg_rect   = D2D1::RectF(0.f, 0.f,
                                                   static_cast<float>(kOvW),
                                                   static_cast<float>(kOvH));
        const D2D1_RECT_F text_rect = D2D1::RectF(6.f, 2.f,
                                                   static_cast<float>(kOvW) - 4.f,
                                                   static_cast<float>(kOvH) - 2.f);
        g_d2d_ctx->BeginDraw();
        g_d2d_ctx->SetTransform(D2D1::Matrix3x2F::Identity());
        g_d2d_ctx->FillRectangle(bg_rect,   g_brush_bg);
        g_d2d_ctx->DrawText(g_overlay_text,
                            static_cast<UINT32>(::wcslen(g_overlay_text)),
                            g_fmt, text_rect, g_brush_text);
        if (FAILED(g_d2d_ctx->EndDraw())) {
            // Helper device lost; give up on compositing for remaining frames.
            --g_overlay_frames;
            return;
        }

        // --- 2. CPU readback from helper staging ---
        g_helper_ctx->CopyResource(g_helper_staging, g_helper_rt);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(g_helper_ctx->Map(g_helper_staging, 0, D3D11_MAP_READ, 0, &mapped))) {
            --g_overlay_frames;
            return;
        }

        // --- 3. Get game's device + context ---
        ID3D11Device*        game_dev = nullptr;
        ID3D11DeviceContext*  game_ctx = nullptr;
        if (FAILED(sc->GetDevice(__uuidof(ID3D11Device),
                                 reinterpret_cast<void**>(&game_dev))) || !game_dev) {
            g_helper_ctx->Unmap(g_helper_staging, 0);
            --g_overlay_frames;
            return;
        }
        game_dev->GetImmediateContext(&game_ctx);

        // --- 4. Upload pixels to game-side B8G8R8A8 overlay texture ---
        game_ctx->UpdateSubresource(g_overlay_tex, 0, nullptr,
                                    mapped.pData, mapped.RowPitch, 0);
        g_helper_ctx->Unmap(g_helper_staging, 0);

        // --- 5. Get current back buffer and create a per-frame RTV ---
        ID3D11Texture2D* bb = nullptr;
        if (FAILED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D),
                                 reinterpret_cast<void**>(&bb))) || !bb) {
            game_ctx->Release();
            game_dev->Release();
            --g_overlay_frames;
            return;
        }

        ID3D11RenderTargetView* temp_rtv = nullptr;
        if (FAILED(game_dev->CreateRenderTargetView(bb, nullptr, &temp_rtv))) {
            bb->Release();
            game_ctx->Release();
            game_dev->Release();
            --g_overlay_frames;
            return;
        }

        D3D11_TEXTURE2D_DESC bb_desc{};
        bb->GetDesc(&bb_desc);
        bb->Release();

        // --- 6. Save game pipeline state ---
        SavedState saved{};
        save_state(game_ctx, saved);

        // --- 7. Set overlay pipeline ---
        const D3D11_VIEWPORT vp{
            0.f, 0.f,
            static_cast<float>(bb_desc.Width),
            static_cast<float>(bb_desc.Height),
            0.f, 1.f
        };
        const D3D11_RECT scissor{
            static_cast<LONG>(kOvX), static_cast<LONG>(kOvY),
            static_cast<LONG>(kOvX + kOvW), static_cast<LONG>(kOvY + kOvH)
        };
        const FLOAT kBlendFactor[4]{};
        ID3D11ShaderResourceView* nullsrv = nullptr;

        game_ctx->IASetInputLayout(nullptr);
        game_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        game_ctx->IASetVertexBuffers(0, 0, nullptr, nullptr, nullptr);
        game_ctx->IASetIndexBuffer(nullptr, DXGI_FORMAT_UNKNOWN, 0);
        game_ctx->VSSetShader(g_vs, nullptr, 0);
        game_ctx->PSSetShader(g_ps, nullptr, 0);
        game_ctx->PSSetShaderResources(0, 1, &g_overlay_srv);
        game_ctx->PSSetSamplers(0, 1, &g_sampler);
        game_ctx->GSSetShader(nullptr, nullptr, 0);
        game_ctx->HSSetShader(nullptr, nullptr, 0);
        game_ctx->DSSetShader(nullptr, nullptr, 0);
        game_ctx->OMSetRenderTargets(1, &temp_rtv, nullptr);
        game_ctx->OMSetBlendState(g_blend, kBlendFactor, 0xFFFFFFFF);
        game_ctx->OMSetDepthStencilState(g_dss, 0);
        game_ctx->RSSetState(g_rasterizer);
        game_ctx->RSSetViewports(1, &vp);
        game_ctx->RSSetScissorRects(1, &scissor);

        // --- 8. Draw fullscreen triangle (scissored to overlay rect) ---
        game_ctx->Draw(3, 0);

        // Unbind our RTV and SRV before restoring (avoid leftover bindings).
        game_ctx->OMSetRenderTargets(0, nullptr, nullptr);
        game_ctx->PSSetShaderResources(0, 1, &nullsrv);

        // --- 9. Restore game pipeline state ---
        restore_state(game_ctx, saved);

        temp_rtv->Release();
        game_ctx->Release();
        game_dev->Release();

    } else if (g_fallback_active && g_fallback_hwnd) {
        // Window-title fallback: already set in overlay_notify; just let it count down.
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

    release_pipeline();
    release_text();

    if (g_d3dcompiler_dll) {
        ::FreeLibrary(g_d3dcompiler_dll);
        g_d3dcompiler_dll = nullptr;
        g_d3dcompile      = nullptr;
    }

    g_init_attempted = false;
    g_text_ok        = false;
    g_composite_ok   = false;
    g_fallback_active = false;
    g_overlay_frames  = 0;
}

} // namespace openripper::backends::d3d11
