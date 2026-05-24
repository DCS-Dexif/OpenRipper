// OpenRipper - src/backends/d3d11/hooks_d3d11.cpp
//
// D3D11 / DXGI capture backend (Stage 2).
//
// WHAT THIS FILE DOES
// -------------------
// Hooks six COM methods via MinHook inline trampolines so the backend can:
//   1. Observe per-frame draw activity (Stage 1 behaviour, kept).
//   2. Record every ID3D11InputLayout created by the app so we can later
//      decode vertex attribute layout without reflection (Stage 2).
//   3. On the designated capture frame (set by OpenRipper.cfg), call
//      capture_draw() for every Draw* invocation, receive a MeshSnapshot,
//      and write it to an OBJ file in the output directory (Stage 2).
//
// HOOKED METHODS
//   IDXGISwapChain          vtable[8]  = Present
//   ID3D11Device            vtable[11] = CreateInputLayout
//   ID3D11DeviceContext     vtable[12] = DrawIndexed
//   ID3D11DeviceContext     vtable[13] = Draw
//   ID3D11DeviceContext     vtable[20] = DrawIndexedInstanced
//   ID3D11DeviceContext     vtable[21] = DrawInstanced
//
// VTABLE-SCAN TECHNIQUE
// ---------------------
//   1. Create a tiny throw-away device + swap chain at backend init.
//   2. Read function pointers directly out of the COM object vtables.
//   3. Install MinHook trampolines at those addresses.
//   4. Release the temporary objects. The addresses remain valid because
//      vtables live in d3d11.dll / dxgi.dll module memory, not objects.
//
// CAPTURE TRIGGER (Stage 2)
// -------------------------
//   Read 'capture_frame=N' from OpenRipper.cfg (via runtime_state.hpp).
//   Frame N's draws are captured between Present(N-1) and Present(N).
//   All output lands in g_output_dir / "frame######_draw#####.obj".
//   Stage 4 will replace this with a hotkey; the g_capture_active atomic
//   is the common interface that hotkey code will also flip.

#include "hooks_d3d11.hpp"
#include "capture_d3d11.hpp"
#include "state_d3d11.hpp"
#include "runtime_state.hpp"

#include "core/logger.hpp"
#include "exporters/obj_exporter.hpp"

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>

#include <MinHook.h>

#include <atomic>
#include <cstddef>
#include <filesystem>
#include <limits>
#include <mutex>

namespace openripper::backends::d3d11 {
namespace {

// ---- vtable indices ---------------------------------------------------------
//
// Slots are stable across every Windows 10/11 SDK and are widely documented
// (RenderDoc, ReShade, SpecialK, PIX all use the same numbers). Derived from
// the COM interface inheritance chain:
//
//   IUnknown                 = 0..2
//   IDXGIObject              = 3..6
//   IDXGIDeviceSubObject     = 7
//   IDXGISwapChain           = 8..   (Present @ 8)
//
//   IUnknown                 = 0..2
//   ID3D11Device             = 3..   (CreateBuffer @ 3, CreateInputLayout @ 11)
//
//   IUnknown                 = 0..2
//   ID3D11DeviceChild        = 3..6
//   ID3D11DeviceContext      = 7..   (DrawIndexed @ 12, Draw @ 13,
//                                     DrawIndexedInstanced @ 20,
//                                     DrawInstanced @ 21)
constexpr std::size_t k_vt_present              = 8;
constexpr std::size_t k_vt_create_input_layout  = 11;
constexpr std::size_t k_vt_draw_indexed         = 12;
constexpr std::size_t k_vt_draw                 = 13;
constexpr std::size_t k_vt_draw_indexed_inst    = 20;
constexpr std::size_t k_vt_draw_instanced       = 21;

// ---- Function pointer types -------------------------------------------------
using Present_t              = HRESULT (STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using CreateInputLayout_t    = HRESULT (STDMETHODCALLTYPE*)(ID3D11Device*,
                                   const D3D11_INPUT_ELEMENT_DESC*, UINT,
                                   const void*, SIZE_T, ID3D11InputLayout**);
using Draw_t                 = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT);
using DrawIndexed_t          = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, INT);
using DrawInstanced_t        = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT, UINT);
using DrawIdxInstanced_t     = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT, INT, UINT);

// MinHook trampolines — call these to reach the original (unhooked) function.
Present_t           g_real_present             = nullptr;
CreateInputLayout_t g_real_create_input_layout = nullptr;
Draw_t              g_real_draw                = nullptr;
DrawIndexed_t       g_real_draw_indexed        = nullptr;
DrawInstanced_t     g_real_draw_instanced      = nullptr;
DrawIdxInstanced_t  g_real_draw_idx_inst       = nullptr;

// ---- Per-frame counters (Stage 1 diagnostics) -------------------------------
std::atomic<std::uint64_t> g_frame_counter{0};
std::atomic<std::uint64_t> g_draws_this_frame{0};

// ---- Capture state (Stage 2) ------------------------------------------------
// g_capture_active: set by hooked_present when it's time to capture.
//   Stage 4 hotkey code will also write this (same interface, different writer).
// g_capture_draw_idx: monotonic draw counter within the current capture frame.
std::atomic<bool>          g_capture_active{false};
std::atomic<std::uint32_t> g_capture_draw_idx{0};

std::atomic<bool>  g_installed{false};
std::mutex         g_install_mu;

// ---- Helper: run capture for one draw call ----------------------------------
// Calls capture_draw (GPU->CPU readback), then write_obj. Wrapped in try/catch
// so a broken draw cannot crash the host.
void try_capture(ID3D11DeviceContext* ctx,
                 std::uint32_t index_count,
                 std::uint32_t vertex_count,
                 std::uint32_t start_index,
                 std::int32_t  base_vertex)
{
    const auto draw_id  = g_capture_draw_idx.fetch_add(1, std::memory_order_relaxed);
    const auto frame_id = static_cast<std::uint32_t>(
                              g_frame_counter.load(std::memory_order_relaxed));
    try {
        auto snap = capture_draw(ctx, index_count, vertex_count,
                                 start_index, base_vertex, draw_id, frame_id);
        if (!snap) return;   // capture_draw already logged the reason

        const std::filesystem::path out = g_output_dir / (snap->name + ".obj");
        if (exporters::write_obj(*snap, out))
            OR_LOG_INFO("capture: wrote {}", out.filename().string());
    } catch (...) {
        OR_LOG_WARN("capture: exception in draw {} - skipping", draw_id);
    }
}

// ---- Hook bodies ------------------------------------------------------------

HRESULT STDMETHODCALLTYPE hooked_present(IDXGISwapChain* sc, UINT sync_interval, UINT flags) {
    // 'frame' = 0-based index of the frame we just rendered (before increment).
    const auto frame  = g_frame_counter.fetch_add(1, std::memory_order_relaxed);
    const auto draws  = g_draws_this_frame.exchange(0, std::memory_order_relaxed);
    const auto target = g_capture_frame_target.load(std::memory_order_relaxed);

    // Throttle-logged diagnostic (60-frame cadence keeps 144 Hz titles quiet).
    if ((frame % 60) == 0)
        OR_LOG_DEBUG("frame {} - {} draw calls in last frame", frame, draws);

    // End of the capture frame → stop capturing.
    if (g_capture_active.load(std::memory_order_relaxed) && frame == target) {
        g_capture_active.store(false, std::memory_order_release);
        OR_LOG_INFO("capture: frame {:06} complete ({} draws written)",
                    target, g_capture_draw_idx.load(std::memory_order_relaxed));
    }

    // Next frame is the capture frame → pre-activate for its draws.
    // Guard against UINT64_MAX (disabled) wrapping around to frame + 1 == 0.
    if (target != std::numeric_limits<std::uint64_t>::max() && frame + 1 == target) {
        g_capture_draw_idx.store(0, std::memory_order_relaxed);
        g_capture_active.store(true, std::memory_order_release);
        OR_LOG_INFO("capture: frame {:06} begin - capturing all draws", target);
    }

    return g_real_present(sc, sync_interval, flags);
}

HRESULT STDMETHODCALLTYPE hooked_create_input_layout(
    ID3D11Device*                   dev,
    const D3D11_INPUT_ELEMENT_DESC* desc,
    UINT                            num_elements,
    const void*                     bytecode,
    SIZE_T                          bytecode_len,
    ID3D11InputLayout**             pp_layout)
{
    HRESULT hr = g_real_create_input_layout(dev, desc, num_elements, bytecode, bytecode_len, pp_layout);
    if (SUCCEEDED(hr) && pp_layout && *pp_layout)
        register_input_layout(*pp_layout, desc, num_elements);
    return hr;
}

void STDMETHODCALLTYPE hooked_draw(ID3D11DeviceContext* ctx,
                                   UINT vertex_count, UINT start_vertex) {
    g_real_draw(ctx, vertex_count, start_vertex);
    g_draws_this_frame.fetch_add(1, std::memory_order_relaxed);
    if (g_capture_active.load(std::memory_order_acquire))
        try_capture(ctx, 0, vertex_count, 0, static_cast<std::int32_t>(start_vertex));
}

void STDMETHODCALLTYPE hooked_draw_indexed(ID3D11DeviceContext* ctx,
                                           UINT index_count, UINT start_index, INT base_vertex) {
    g_real_draw_indexed(ctx, index_count, start_index, base_vertex);
    g_draws_this_frame.fetch_add(1, std::memory_order_relaxed);
    if (g_capture_active.load(std::memory_order_acquire))
        try_capture(ctx, index_count, 0, start_index, base_vertex);
}

void STDMETHODCALLTYPE hooked_draw_instanced(ID3D11DeviceContext* ctx,
                                             UINT vertex_count_per_instance,
                                             UINT instance_count,
                                             UINT start_vertex_location,
                                             UINT start_instance_location) {
    g_real_draw_instanced(ctx, vertex_count_per_instance, instance_count,
                          start_vertex_location, start_instance_location);
    g_draws_this_frame.fetch_add(1, std::memory_order_relaxed);
    if (g_capture_active.load(std::memory_order_acquire))
        // Capture all geometry from slot 0; instancing handled by picking the
        // first instance (base_vertex == start_vertex_location for vert data).
        try_capture(ctx, 0, vertex_count_per_instance, 0,
                    static_cast<std::int32_t>(start_vertex_location));
}

void STDMETHODCALLTYPE hooked_draw_idx_instanced(ID3D11DeviceContext* ctx,
                                                 UINT index_count_per_instance,
                                                 UINT instance_count,
                                                 UINT start_index_location,
                                                 INT  base_vertex_location,
                                                 UINT start_instance_location) {
    g_real_draw_idx_inst(ctx, index_count_per_instance, instance_count,
                         start_index_location, base_vertex_location,
                         start_instance_location);
    g_draws_this_frame.fetch_add(1, std::memory_order_relaxed);
    if (g_capture_active.load(std::memory_order_acquire))
        try_capture(ctx, index_count_per_instance, 0,
                    start_index_location, base_vertex_location);
}

// ---- vtable acquisition -----------------------------------------------------

struct VTableAddrs {
    void* present              = nullptr;
    void* create_input_layout  = nullptr;
    void* draw                 = nullptr;
    void* draw_indexed         = nullptr;
    void* draw_instanced       = nullptr;
    void* draw_idx_inst        = nullptr;
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
    void** vt_dev = *reinterpret_cast<void***>(dev);
    void** vt_ctx = *reinterpret_cast<void***>(ctx);

    out.present             = vt_sc [k_vt_present];
    out.create_input_layout = vt_dev[k_vt_create_input_layout];
    out.draw                = vt_ctx[k_vt_draw];
    out.draw_indexed        = vt_ctx[k_vt_draw_indexed];
    out.draw_instanced      = vt_ctx[k_vt_draw_instanced];
    out.draw_idx_inst       = vt_ctx[k_vt_draw_indexed_inst];

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

// ---- Public API -------------------------------------------------------------

// Called from dllmain's init_thread once the config has been loaded and
// g_capture_frame_target / g_output_dir in runtime_state.hpp are set.
void activate_capture_if_frame_zero() {
    // Special case: target=0 means capture starts before the very first
    // Present, so we need to arm g_capture_active before install_hooks returns.
    if (g_capture_frame_target.load(std::memory_order_relaxed) == 0) {
        g_capture_draw_idx.store(0, std::memory_order_relaxed);
        g_capture_active.store(true, std::memory_order_release);
        OR_LOG_INFO("capture: frame 0 targeted - capturing from first draw");
    }
}

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
    ok &= create_one_hook(addrs.present,
                          reinterpret_cast<void*>(&hooked_present),
                          reinterpret_cast<void**>(&g_real_present),
                          "Present");
    ok &= create_one_hook(addrs.create_input_layout,
                          reinterpret_cast<void*>(&hooked_create_input_layout),
                          reinterpret_cast<void**>(&g_real_create_input_layout),
                          "CreateInputLayout");
    ok &= create_one_hook(addrs.draw,
                          reinterpret_cast<void*>(&hooked_draw),
                          reinterpret_cast<void**>(&g_real_draw),
                          "Draw");
    ok &= create_one_hook(addrs.draw_indexed,
                          reinterpret_cast<void*>(&hooked_draw_indexed),
                          reinterpret_cast<void**>(&g_real_draw_indexed),
                          "DrawIndexed");
    ok &= create_one_hook(addrs.draw_instanced,
                          reinterpret_cast<void*>(&hooked_draw_instanced),
                          reinterpret_cast<void**>(&g_real_draw_instanced),
                          "DrawInstanced");
    ok &= create_one_hook(addrs.draw_idx_inst,
                          reinterpret_cast<void*>(&hooked_draw_idx_instanced),
                          reinterpret_cast<void**>(&g_real_draw_idx_inst),
                          "DrawIndexedInstanced");

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
    OR_LOG_INFO("D3D11 hooks installed (Present + CreateInputLayout + 4 Draw variants).");
    return true;
}

void remove_hooks() {
    std::lock_guard lk(g_install_mu);
    if (!g_installed.load(std::memory_order_relaxed)) return;
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();
    forget_all_input_layouts();
    g_installed.store(false, std::memory_order_release);
    OR_LOG_INFO("D3D11 hooks removed.");
}

} // namespace openripper::backends::d3d11
