// OpenRipper - src/backends/d3d11/hooks_d3d11.cpp
//
// D3D11 / DXGI capture backend (Stage 4).
//
// WHAT THIS FILE DOES
// -------------------
// Hooks six COM methods via MinHook inline trampolines so the backend can:
//   1. Observe per-frame draw activity (Stage 1 behaviour, kept).
//   2. Record every ID3D11InputLayout created by the app so we can later
//      decode vertex attribute layout without reflection (Stage 2).
//   3. On the configured capture trigger (config frame or F10 hotkey),
//      capture mesh + textures for every Draw* call in the target frame(s),
//      and write OBJ / DDS / PNG / JSON files (Stages 2-3).
//   4. Show a D2D1 text overlay confirming capture (Stage 4).
//
// HOOKED METHODS
//   IDXGISwapChain          vtable[8]  = Present
//   ID3D11Device            vtable[11] = CreateInputLayout
//   ID3D11DeviceContext     vtable[12] = DrawIndexed
//   ID3D11DeviceContext     vtable[13] = Draw
//   ID3D11DeviceContext     vtable[20] = DrawIndexedInstanced
//   ID3D11DeviceContext     vtable[21] = DrawInstanced
//
// CAPTURE STATE MACHINE (Stage 4 — unified freeze counter)
// ---------------------------------------------------------
//   Both the config trigger and the F10 hotkey thread write
//   g_freeze_frames_remaining (atomic uint32). hooked_present:
//
//   1. END-OF-FRAME: if capture is active, flush manifest, decrement counter.
//      If counter hits 0: deactivate, notify overlay, append session record.
//      Otherwise: reset draw counter for the next burst frame.
//
//   2. ARM (config): if frame+1 == target, store g_freeze_count into the
//      counter and activate.
//
//   3. ARM (hotkey): if counter > 0 and not already active, activate.
//
//   4. Draw overlay, then call original Present.

#include "hooks_d3d11.hpp"
#include "capture_d3d11.hpp"
#include "state_d3d11.hpp"
#include "runtime_state.hpp"
#include "overlay_d3d11.hpp"

#include "core/hotkey.hpp"
#include "core/logger.hpp"
#include "exporters/obj_exporter.hpp"
#include "exporters/dds_exporter.hpp"
#include "exporters/png_exporter.hpp"
#include "exporters/material_exporter.hpp"
#include "exporters/session_exporter.hpp"

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <dxgi1_2.h>

#include <MinHook.h>

#include <atomic>
#include <cstddef>
#include <filesystem>
#include <format>
#include <limits>
#include <mutex>
#include <vector>

namespace openripper::backends::d3d11 {
namespace {

// ---- vtable indices ---------------------------------------------------------
constexpr std::size_t k_vt_present              = 8;
constexpr std::size_t k_vt_present1             = 22; // IDXGISwapChain1 (flip-model games)
constexpr std::size_t k_vt_create_input_layout  = 11;
constexpr std::size_t k_vt_draw_indexed         = 12;
constexpr std::size_t k_vt_draw                 = 13;
constexpr std::size_t k_vt_draw_indexed_inst    = 20;
constexpr std::size_t k_vt_draw_instanced       = 21;

// ---- Function pointer types -------------------------------------------------
using Present_t              = HRESULT (STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using Present1_t             = HRESULT (STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
using CreateInputLayout_t    = HRESULT (STDMETHODCALLTYPE*)(ID3D11Device*,
                                   const D3D11_INPUT_ELEMENT_DESC*, UINT,
                                   const void*, SIZE_T, ID3D11InputLayout**);
using Draw_t                 = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT);
using DrawIndexed_t          = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, INT);
using DrawInstanced_t        = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT, UINT);
using DrawIdxInstanced_t     = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT, INT, UINT);

// MinHook trampolines.
Present_t           g_real_present             = nullptr;
Present1_t          g_real_present1            = nullptr;
CreateInputLayout_t g_real_create_input_layout = nullptr;
Draw_t              g_real_draw                = nullptr;
DrawIndexed_t       g_real_draw_indexed        = nullptr;
DrawInstanced_t     g_real_draw_instanced      = nullptr;
DrawIdxInstanced_t  g_real_draw_idx_inst       = nullptr;

// ---- Per-frame counters (Stage 1) ------------------------------------------
std::atomic<std::uint64_t> g_frame_counter{0};
std::atomic<std::uint64_t> g_draws_this_frame{0};

// ---- Capture state ---------------------------------------------------------
std::atomic<bool>          g_capture_active{false};
std::atomic<std::uint32_t> g_capture_draw_idx{0};
std::vector<exporters::DrawMaterialRecord> g_pending_materials;

// ---- Session frame accumulator ---------------------------------------------
std::vector<exporters::FrameRecord> g_session_frames;

std::atomic<bool> g_installed{false};
std::mutex        g_install_mu;

// ---- try_capture -----------------------------------------------------------
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
        exporters::DrawMaterialRecord rec;
        rec.draw_id = draw_id;

        // ---- Mesh ----
        auto snap = capture_draw(ctx, index_count, vertex_count,
                                 start_index, base_vertex, draw_id, frame_id);
        if (snap) {
            const std::filesystem::path obj_out = g_output_dir / (snap->name + ".obj");
            if (exporters::write_obj(*snap, obj_out, g_flip_winding)) {
                OR_LOG_INFO("capture: wrote {}", obj_out.filename().string());
                rec.mesh_file = obj_out.filename().string();
            }
        }

        // ---- Textures ----
        auto textures = capture_pixel_textures(ctx, draw_id, frame_id);
        for (auto& tr : textures) {
            std::string filename;
            std::uint32_t fmt{0}, w{0}, h{0}, mips{0};

            if (!tr.reuse_file.empty()) {
                // Duplicate texture — reuse the already-written file.
                filename = tr.reuse_file;
                fmt = tr.reuse_fmt; w = tr.reuse_w; h = tr.reuse_h; mips = tr.reuse_mips;
            } else {
                std::filesystem::path tp = g_output_dir / (tr.snap.name + ".png");
                bool written = exporters::write_png(tr.snap, tp);
                if (!written) {
                    tp = g_output_dir / (tr.snap.name + ".dds");
                    written = exporters::write_dds(tr.snap, tp);
                }
                if (written) {
                    filename = tp.filename().string();
                    fmt  = tr.snap.native_format;
                    w    = tr.snap.width;
                    h    = tr.snap.height;
                    mips = tr.snap.mip_levels;
                    if (g_dedup && tr.source_ptr)
                        dedup_tex_register(tr.source_ptr, filename, tr.snap);
                }
            }

            if (!filename.empty()) {
                exporters::DrawMaterialRecord::Tex t;
                t.slot          = tr.slot;
                t.file          = filename;
                t.native_format = fmt;
                t.width         = w;
                t.height        = h;
                t.mips          = mips;
                rec.ps_textures.push_back(std::move(t));
            }
        }

        g_pending_materials.push_back(std::move(rec));

    } catch (...) {
        OR_LOG_WARN("capture: exception in draw {} - skipping", draw_id);
    }
}

// ---- flush_frame -----------------------------------------------------------
// Flushes pending material records to JSON and appends a session frame entry.
// Returns the draw count (used for the overlay notification).
std::uint32_t flush_frame(std::uint64_t frame) {
    const auto draw_count = static_cast<std::uint32_t>(g_pending_materials.size());

    if (!g_pending_materials.empty()) {
        const auto json_path = g_output_dir /
            std::format("frame{:06}_materials.json", frame);
        exporters::write_material_manifest(
            static_cast<std::uint32_t>(frame), g_pending_materials, json_path);
        g_session_frames.push_back({
            static_cast<std::uint32_t>(frame),
            draw_count,
            json_path.filename().string()
        });
        g_pending_materials.clear();
    }

    OR_LOG_INFO("capture: frame {:06} complete ({} draws written)", frame, draw_count);
    return draw_count;
}

// ---- Hook bodies ------------------------------------------------------------

// Shared state machine for both Present and Present1.
// Returns true if the caller should suppress the real Present (time_freeze_on_rip).
bool present_shared(IDXGISwapChain* sc) {
    const auto frame  = g_frame_counter.fetch_add(1, std::memory_order_relaxed);
    const auto draws  = g_draws_this_frame.exchange(0, std::memory_order_relaxed);
    const auto target = g_capture_frame_target.load(std::memory_order_relaxed);

    if ((frame % 60) == 0)
        OR_LOG_DEBUG("frame {} - {} draw calls in last frame", frame, draws);

    // === 1. End-of-frame: flush + decrement freeze counter ===
    if (g_capture_active.load(std::memory_order_relaxed)) {
        const auto draw_count = flush_frame(frame);

        const auto rem = g_freeze_frames_remaining.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (rem == 0) {
            g_capture_active.store(false, std::memory_order_release);
            overlay_notify(frame, draw_count);
        } else {
            g_capture_draw_idx.store(0, std::memory_order_relaxed);
            dedup_begin_frame(); // clear dedup map for the next burst frame
            OR_LOG_INFO("capture: {} more frame(s) to go", rem);
        }
    }

    // === 2. Arm: config trigger (pre-activate for the target frame) ===
    if (target != std::numeric_limits<std::uint64_t>::max() &&
        frame + 1 == target &&
        !g_capture_active.load(std::memory_order_relaxed))
    {
        g_freeze_frames_remaining.store(g_freeze_count, std::memory_order_release);
        g_capture_draw_idx.store(0, std::memory_order_relaxed);
        g_pending_materials.clear();
        dedup_begin_frame();
        g_capture_active.store(true, std::memory_order_release);
        OR_LOG_INFO("capture: frame {:06} begin - capturing {} frame(s)",
                    target, g_freeze_count);
    }

    // === 3. Arm: hotkey trigger (counter set by hotkey thread) ===
    if (g_freeze_frames_remaining.load(std::memory_order_acquire) > 0 &&
        !g_capture_active.load(std::memory_order_relaxed))
    {
        g_capture_draw_idx.store(0, std::memory_order_relaxed);
        g_pending_materials.clear();
        dedup_begin_frame();
        g_capture_active.store(true, std::memory_order_release);
        OR_LOG_INFO("hotkey: frame {:06} begin - capturing {} frame(s)",
                    frame + 1, g_freeze_frames_remaining.load(std::memory_order_relaxed));
    }

    overlay_draw(sc, frame);
    return g_capture_active.load(std::memory_order_relaxed) && g_time_freeze_on_rip;
}

HRESULT STDMETHODCALLTYPE hooked_present(IDXGISwapChain* sc, UINT sync_interval, UINT flags) {
    if (present_shared(sc)) return S_OK;
    return g_real_present(sc, sync_interval, flags);
}

HRESULT STDMETHODCALLTYPE hooked_present1(IDXGISwapChain1* sc, UINT sync_interval, UINT flags,
                                           const DXGI_PRESENT_PARAMETERS* pp) {
    if (present_shared(sc)) return S_OK;
    return g_real_present1(sc, sync_interval, flags, pp);
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
    void* present1             = nullptr; // IDXGISwapChain1 (nullptr if not supported)
    void* create_input_layout  = nullptr;
    void* draw                 = nullptr;
    void* draw_indexed         = nullptr;
    void* draw_instanced       = nullptr;
    void* draw_idx_inst        = nullptr;
};

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
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        want, _countof(want), D3D11_SDK_VERSION,
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

    // IDXGISwapChain1::Present1 — available on DXGI 1.2+ (Win 8+).
    IDXGISwapChain1* sc1 = nullptr;
    if (SUCCEEDED(sc->QueryInterface(IID_PPV_ARGS(&sc1)))) {
        void** vt_sc1 = *reinterpret_cast<void***>(sc1);
        out.present1 = vt_sc1[k_vt_present1];
        sc1->Release();
    } else {
        OR_LOG_WARN("vtable-scan: IDXGISwapChain1 QI failed - Present1 hook skipped");
    }

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

void activate_capture_if_frame_zero() {
    if (g_capture_frame_target.load(std::memory_order_relaxed) == 0) {
        g_freeze_frames_remaining.store(g_freeze_count, std::memory_order_release);
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
    if (!acquire_vtable_addrs(addrs)) { MH_Uninitialize(); return false; }

    bool ok = true;
    ok &= create_one_hook(addrs.present,
                          reinterpret_cast<void*>(&hooked_present),
                          reinterpret_cast<void**>(&g_real_present),
                          "Present");
    if (addrs.present1)
        ok &= create_one_hook(addrs.present1,
                              reinterpret_cast<void*>(&hooked_present1),
                              reinterpret_cast<void**>(&g_real_present1),
                              "Present1");
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

    if (!ok) { MH_Uninitialize(); return false; }

    if (MH_EnableHook(MH_ALL_HOOKS) != MH_OK) {
        OR_LOG_ERROR("MH_EnableHook(MH_ALL_HOOKS) failed");
        MH_Uninitialize();
        return false;
    }

    g_installed.store(true, std::memory_order_release);
    OR_LOG_INFO("D3D11 hooks installed (Present{} + CreateInputLayout + 4 Draw variants).",
                addrs.present1 ? "+Present1" : "");
    return true;
}

void remove_hooks() {
    std::lock_guard lk(g_install_mu);
    if (!g_installed.load(std::memory_order_relaxed)) return;

    openripper::stop_hotkey_thread();
    overlay_shutdown();

    if (!g_session_frames.empty()) {
        exporters::write_session_manifest(
            g_session_id, g_target_exe_name,
            static_cast<std::uint32_t>(::GetCurrentProcessId()),
            g_session_frames,
            g_output_dir / "session.json");
        g_session_frames.clear();
    }

    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();
    forget_all_input_layouts();
    g_pending_materials.clear();
    g_installed.store(false, std::memory_order_release);
    OR_LOG_INFO("D3D11 hooks removed.");
}

void flush_session_manifest_now() {
    if (g_session_frames.empty()) return;
    exporters::write_session_manifest(
        g_session_id, g_target_exe_name,
        static_cast<std::uint32_t>(::GetCurrentProcessId()),
        g_session_frames,
        g_output_dir / "session.json");
    g_session_frames.clear();
}

} // namespace openripper::backends::d3d11
