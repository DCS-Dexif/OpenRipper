// OpenRipper - src/backends/d3d11/hooks_d3d11.cpp
//
// D3D11 / DXGI capture backend (Stage 1).
//
// This file installs inline hooks on a handful of well-known vtable slots so
// the backend can observe per-frame draw activity. Stage 1 is intentionally
// passive: every hook simply counts/logs and forwards to the real function.
// Vertex / index capture, texture dumping and the rip hotkey are scheduled
// for later stages (see docs/ROADMAP.md).
//
// vtable-scan technique:
//   1. Create a tiny throw-away device + swap chain at backend init.
//   2. Read function pointers directly out of the COM object vtables.
//   3. Install MinHook trampolines at those addresses.
//   4. Release the temporary objects. The addresses remain valid because
//      vtables live in d3d11.dll / dxgi.dll module memory, not in the
//      objects themselves.

#include "hooks_d3d11.hpp"

#include "core/logger.hpp"

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>

#include <MinHook.h>

#include <atomic>
#include <cstddef>
#include <mutex>

namespace openripper::backends::d3d11 {
namespace {

// ---- vtable indices ---------------------------------------------------------
//
// Slots are stable across every Windows 10/11 SDK and are widely documented
// (RenderDoc, ReShade, SpecialK and PIX all use the same numbers). They are
// derived from the COM interface inheritance chain:
//
//   IUnknown                 = 0..2
//   IDXGIObject              = 3..6
//   IDXGIDeviceSubObject     = 7
//   IDXGISwapChain           = 8.. (Present @ 8)
//
//   IUnknown                 = 0..2
//   ID3D11DeviceChild        = 3..6
//   ID3D11DeviceContext      = 7..   (DrawIndexed @ 12, Draw @ 13,
//                                     DrawIndexedInstanced @ 20,
//                                     DrawInstanced @ 21)
constexpr std::size_t k_vt_present              = 8;
constexpr std::size_t k_vt_draw_indexed         = 12;
constexpr std::size_t k_vt_draw                 = 13;
constexpr std::size_t k_vt_draw_indexed_inst    = 20;
constexpr std::size_t k_vt_draw_instanced       = 21;

// ---- Function pointer types -------------------------------------------------
using Present_t           = HRESULT (STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using Draw_t              = void    (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT);
using DrawIndexed_t       = void    (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, INT);
using DrawInstanced_t     = void    (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT, UINT);
using DrawIdxInstanced_t  = void    (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT, INT, UINT);

// Trampolines populated by MinHook.
Present_t          g_real_present        = nullptr;
Draw_t             g_real_draw           = nullptr;
DrawIndexed_t      g_real_draw_indexed   = nullptr;
DrawInstanced_t    g_real_draw_instanced = nullptr;
DrawIdxInstanced_t g_real_draw_idx_inst  = nullptr;

std::atomic<std::uint64_t> g_frame_counter{0};
std::atomic<std::uint64_t> g_draws_this_frame{0};
std::atomic<bool>          g_installed{false};
std::mutex                 g_install_mu;

// ---- Hook bodies ------------------------------------------------------------

HRESULT STDMETHODCALLTYPE hooked_present(IDXGISwapChain* sc, UINT sync_interval, UINT flags) {
    const auto draws = g_draws_this_frame.exchange(0, std::memory_order_relaxed);
    const auto frame = g_frame_counter.fetch_add(1, std::memory_order_relaxed);

    // Throttle: 60-frame cadence so 144 Hz titles don't flood the log.
    if ((frame % 60) == 0) {
        OR_LOG_DEBUG("frame {} - {} draw calls in last frame", frame, draws);
    }

    return g_real_present(sc, sync_interval, flags);
}

void STDMETHODCALLTYPE hooked_draw(ID3D11DeviceContext* ctx,
                                   UINT vertex_count, UINT start_vertex) {
    g_draws_this_frame.fetch_add(1, std::memory_order_relaxed);
    g_real_draw(ctx, vertex_count, start_vertex);
}

void STDMETHODCALLTYPE hooked_draw_indexed(ID3D11DeviceContext* ctx,
                                           UINT index_count, UINT start_index, INT base_vertex) {
    g_draws_this_frame.fetch_add(1, std::memory_order_relaxed);
    g_real_draw_indexed(ctx, index_count, start_index, base_vertex);
}

void STDMETHODCALLTYPE hooked_draw_instanced(ID3D11DeviceContext* ctx,
                                             UINT vertex_count_per_instance,
                                             UINT instance_count,
                                             UINT start_vertex_location,
                                             UINT start_instance_location) {
    g_draws_this_frame.fetch_add(1, std::memory_order_relaxed);
    g_real_draw_instanced(ctx, vertex_count_per_instance, instance_count,
                          start_vertex_location, start_instance_location);
}

void STDMETHODCALLTYPE hooked_draw_idx_instanced(ID3D11DeviceContext* ctx,
                                                 UINT index_count_per_instance,
                                                 UINT instance_count,
                                                 UINT start_index_location,
                                                 INT  base_vertex_location,
                                                 UINT start_instance_location) {
    g_draws_this_frame.fetch_add(1, std::memory_order_relaxed);
    g_real_draw_idx_inst(ctx, index_count_per_instance, instance_count,
                         start_index_location, base_vertex_location,
                         start_instance_location);
}

// ---- vtable acquisition -----------------------------------------------------

struct VTableAddrs {
    void* present           = nullptr;
    void* draw              = nullptr;
    void* draw_indexed      = nullptr;
    void* draw_instanced    = nullptr;
    void* draw_idx_inst     = nullptr;
};

// Register-once minimal window class for the throw-away swap chain.
HWND create_throwaway_window() {
    static const wchar_t* k_class = L"OpenRipperDummyWnd";
    static const bool registered = []() {
        WNDCLASSEXW wc{};
        wc.cbSize        = sizeof(wc);
        wc.lpfnWndProc   = ::DefWindowProcW;
        wc.hInstance     = ::GetModuleHandleW(nullptr);
        wc.lpszClassName = k_class;
        ::RegisterClassExW(&wc);
        return true;
    }();
    (void)registered;
    return ::CreateWindowExW(0, k_class, L"OpenRipperDummy",
                             WS_OVERLAPPEDWINDOW, 0, 0, 1, 1,
                             nullptr, nullptr, ::GetModuleHandleW(nullptr), nullptr);
}

bool acquire_vtable_addrs(VTableAddrs& out) {
    HWND hwnd = create_throwaway_window();
    if (!hwnd) {
        OR_LOG_ERROR("vtable-scan: CreateWindowExW failed (gle={})", ::GetLastError());
        return false;
    }

    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount       = 1;
    sd.BufferDesc.Width  = 1;
    sd.BufferDesc.Height = 1;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage       = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.SampleDesc.Count  = 1;
    sd.OutputWindow      = hwnd;
    sd.Windowed          = TRUE;
    sd.SwapEffect        = DXGI_SWAP_EFFECT_DISCARD;

    const D3D_FEATURE_LEVEL want[] = {
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0,
    };

    D3D_FEATURE_LEVEL    got = {};
    ID3D11Device*        dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    IDXGISwapChain*      sc  = nullptr;

    HRESULT hr = ::D3D11CreateDeviceAndSwapChain(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        0,
        want, _countof(want),
        D3D11_SDK_VERSION,
        &sd, &sc, &dev, &got, &ctx);

    if (FAILED(hr)) {
        OR_LOG_ERROR("vtable-scan: D3D11CreateDeviceAndSwapChain failed (hr=0x{:08X})",
                     static_cast<unsigned>(hr));
        ::DestroyWindow(hwnd);
        return false;
    }

    void** vt_sc  = *reinterpret_cast<void***>(sc);
    void** vt_ctx = *reinterpret_cast<void***>(ctx);

    out.present        = vt_sc [k_vt_present];
    out.draw           = vt_ctx[k_vt_draw];
    out.draw_indexed   = vt_ctx[k_vt_draw_indexed];
    out.draw_instanced = vt_ctx[k_vt_draw_instanced];
    out.draw_idx_inst  = vt_ctx[k_vt_draw_indexed_inst];

    sc->Release();
    ctx->Release();
    dev->Release();
    ::DestroyWindow(hwnd);
    return true;
}

bool create_one_hook(void* target, void* hook, void** trampoline, const char* tag) {
    const MH_STATUS s = MH_CreateHook(target, hook, trampoline);
    if (s != MH_OK) {
        OR_LOG_ERROR("MH_CreateHook({}) failed: {}", tag, static_cast<int>(s));
        return false;
    }
    return true;
}

} // namespace

bool install_hooks() {
    std::lock_guard lk(g_install_mu);
    if (g_installed.load(std::memory_order_relaxed)) return true;

    if (MH_Initialize() != MH_OK) {
        OR_LOG_ERROR("MinHook initialization failed");
        return false;
    }

    VTableAddrs addrs{};
    if (!acquire_vtable_addrs(addrs)) {
        MH_Uninitialize();
        return false;
    }

    bool ok = true;
    ok &= create_one_hook(addrs.present,        reinterpret_cast<void*>(&hooked_present),          reinterpret_cast<void**>(&g_real_present),        "Present");
    ok &= create_one_hook(addrs.draw,           reinterpret_cast<void*>(&hooked_draw),             reinterpret_cast<void**>(&g_real_draw),           "Draw");
    ok &= create_one_hook(addrs.draw_indexed,   reinterpret_cast<void*>(&hooked_draw_indexed),     reinterpret_cast<void**>(&g_real_draw_indexed),   "DrawIndexed");
    ok &= create_one_hook(addrs.draw_instanced, reinterpret_cast<void*>(&hooked_draw_instanced),   reinterpret_cast<void**>(&g_real_draw_instanced), "DrawInstanced");
    ok &= create_one_hook(addrs.draw_idx_inst,  reinterpret_cast<void*>(&hooked_draw_idx_instanced), reinterpret_cast<void**>(&g_real_draw_idx_inst), "DrawIndexedInstanced");

    if (!ok) {
        MH_Uninitialize();
        return false;
    }

    if (MH_EnableHook(MH_ALL_HOOKS) != MH_OK) {
        OR_LOG_ERROR("MH_EnableHook(MH_ALL_HOOKS) failed");
        MH_Uninitialize();
        return false;
    }

    g_installed.store(true, std::memory_order_release);
    OR_LOG_INFO("D3D11 hooks installed (Present + 4 Draw variants).");
    return true;
}

void remove_hooks() {
    std::lock_guard lk(g_install_mu);
    if (!g_installed.load(std::memory_order_relaxed)) return;
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();
    g_installed.store(false, std::memory_order_release);
    OR_LOG_INFO("D3D11 hooks removed.");
}

} // namespace openripper::backends::d3d11
