// OpenRipper - src/backends/d3d9/hooks_d3d9.cpp
//
// Hooks 5 IDirect3DDevice9 vtable methods via MinHook:
//   Present              [17]
//   DrawPrimitive        [81]
//   DrawIndexedPrimitive [82]
//   DrawPrimitiveUP      [83]
//   DrawIndexedPrimitiveUP [84]
//
// Capture state machine is identical to the D3D11 backend. Vertex/index/texture
// data is pulled synchronously inside each Draw hook via device Get* methods
// (no separate state-tracking hooks needed — D3D9 is a fully synchronous API).
//
// Overlay: window-title flash ("CAPTURED — frame NNNNNN") for ~2 s.
// D3D9 has no D2D1 support, and adding D3DX for fonts is a heavy dependency.

#include "hooks_d3d9.hpp"
#include "capture_d3d9.hpp"
#include "runtime_state.hpp"

#include "core/hotkey.hpp"
#include "core/logger.hpp"
#include "exporters/dds_exporter.hpp"
#include "exporters/material_exporter.hpp"
#include "exporters/obj_exporter.hpp"
#include "exporters/png_exporter.hpp"
#include "exporters/session_exporter.hpp"

#include <windows.h>
#include <d3d9.h>
#include <MinHook.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <format>
#include <limits>
#include <mutex>
#include <vector>

namespace openripper::backends::d3d9 {
namespace {

// ---- vtable indices ----------------------------------------------------------
constexpr std::size_t k_vt_present                     = 17;
constexpr std::size_t k_vt_draw_primitive               = 81;
constexpr std::size_t k_vt_draw_indexed_primitive       = 82;
constexpr std::size_t k_vt_draw_primitive_up            = 83;
constexpr std::size_t k_vt_draw_indexed_primitive_up    = 84;

// ---- Function pointer types --------------------------------------------------
// D3D9 Present has no SyncInterval parameter (unlike DXGI).
using Present_t =
    HRESULT (STDMETHODCALLTYPE*)(IDirect3DDevice9*, CONST RECT*, CONST RECT*,
                                  HWND, CONST RGNDATA*);
using DrawPrimitive_t =
    HRESULT (STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT);
using DrawIndexedPrimitive_t =
    HRESULT (STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, INT,
                                  UINT, UINT, UINT, UINT);
using DrawPrimitiveUP_t =
    HRESULT (STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT,
                                  CONST void*, UINT);
using DrawIndexedPrimitiveUP_t =
    HRESULT (STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT,
                                  UINT, UINT, CONST void*, D3DFORMAT,
                                  CONST void*, UINT);

// ---- Trampolines -------------------------------------------------------------
Present_t                g_real_present              = nullptr;
DrawPrimitive_t          g_real_draw_primitive       = nullptr;
DrawIndexedPrimitive_t   g_real_draw_indexed         = nullptr;
DrawPrimitiveUP_t        g_real_draw_primitive_up    = nullptr;
DrawIndexedPrimitiveUP_t g_real_draw_indexed_up      = nullptr;

// ---- Per-frame counters ------------------------------------------------------
std::atomic<std::uint64_t> g_frame_counter{0};
std::atomic<std::uint64_t> g_draws_this_frame{0};

// ---- Capture state -----------------------------------------------------------
std::atomic<bool>          g_capture_active{false};
std::atomic<std::uint32_t> g_capture_draw_idx{0};
std::vector<exporters::DrawMaterialRecord> g_pending_materials;
std::vector<exporters::FrameRecord>        g_session_frames;

std::atomic<bool> g_installed{false};
std::mutex        g_install_mu;

// ---- Overlay (window-title flash) -------------------------------------------
std::atomic<std::uint64_t> g_overlay_until_frame{0};

void overlay_notify(std::uint64_t frame, std::uint32_t draw_count) {
    g_overlay_until_frame.store(frame + 120, std::memory_order_relaxed); // ~2 s at 60 fps
    OR_LOG_INFO("overlay: CAPTURED — frame {:06} ({} draws)", frame, draw_count);
}

void overlay_draw(HWND hwnd, std::uint64_t frame) {
    if (frame < g_overlay_until_frame.load(std::memory_order_relaxed)) {
        ::SetWindowTextW(hwnd, std::format(L"CAPTURED — frame {:06}", frame).c_str());
    }
}

// ---- try_capture -------------------------------------------------------------
void try_capture_indexed(IDirect3DDevice9* dev,
                         D3DPRIMITIVETYPE prim_type,
                         INT base_vertex, UINT min_vertex, UINT num_vertices,
                         UINT start_index, UINT prim_count)
{
    const auto draw_id  = g_capture_draw_idx.fetch_add(1, std::memory_order_relaxed);
    const auto frame_id = static_cast<std::uint32_t>(
                              g_frame_counter.load(std::memory_order_relaxed));
    try {
        exporters::DrawMaterialRecord rec;
        rec.draw_id = draw_id;

        auto mesh = capture_mesh(dev, prim_type, base_vertex, min_vertex, num_vertices,
                                 start_index, prim_count, true, draw_id, frame_id);
        if (mesh) {
            const auto obj_path = g_output_dir / (mesh->name + ".obj");
            if (exporters::write_obj(*mesh, obj_path, g_flip_winding)) {
                OR_LOG_INFO("capture: wrote {}", obj_path.filename().string());
                rec.mesh_file = obj_path.filename().string();
            }
        }

        for (auto& [slot, tex] : capture_textures(dev, draw_id, frame_id)) {
            bool written = false;
            std::filesystem::path tex_path;
            tex_path = g_output_dir / (tex.name + ".png");
            if (exporters::write_png(tex, tex_path)) {
                written = true;
            } else {
                tex_path = g_output_dir / (tex.name + ".dds");
                written = exporters::write_dds(tex, tex_path);
            }
            if (written) {
                exporters::DrawMaterialRecord::Tex t;
                t.slot          = static_cast<std::uint32_t>(slot);
                t.file          = tex_path.filename().string();
                t.native_format = tex.native_format;
                t.width         = tex.width;
                t.height        = tex.height;
                t.mips          = tex.mip_levels;
                rec.ps_textures.push_back(std::move(t));
            }
        }
        g_pending_materials.push_back(std::move(rec));
    } catch (...) {
        OR_LOG_WARN("capture: exception in draw {} - skipping", draw_id);
    }
}

void try_capture_nonindexed(IDirect3DDevice9* dev,
                             D3DPRIMITIVETYPE prim_type,
                             UINT start_vertex, UINT prim_count)
{
    // Reuse the indexed path: base_vertex=start_vertex, min_vertex=0,
    // num_vertices from prim_count, start_index=0, not indexed.
    const auto draw_id  = g_capture_draw_idx.fetch_add(1, std::memory_order_relaxed);
    const auto frame_id = static_cast<std::uint32_t>(
                              g_frame_counter.load(std::memory_order_relaxed));
    try {
        exporters::DrawMaterialRecord rec;
        rec.draw_id = draw_id;

        // Compute vertex count from prim_type + prim_count
        UINT num_verts = 0;
        switch (prim_type) {
        case D3DPT_POINTLIST:     num_verts = prim_count;     break;
        case D3DPT_LINELIST:      num_verts = prim_count * 2; break;
        case D3DPT_LINESTRIP:     num_verts = prim_count + 1; break;
        case D3DPT_TRIANGLELIST:  num_verts = prim_count * 3; break;
        case D3DPT_TRIANGLESTRIP: num_verts = prim_count + 2; break;
        case D3DPT_TRIANGLEFAN:   num_verts = prim_count + 2; break;
        default:                  num_verts = prim_count * 3; break;
        }

        auto mesh = capture_mesh(dev, prim_type,
                                 static_cast<INT>(start_vertex), 0, num_verts,
                                 0, prim_count, false, draw_id, frame_id);
        if (mesh) {
            const auto obj_path = g_output_dir / (mesh->name + ".obj");
            if (exporters::write_obj(*mesh, obj_path, g_flip_winding))
                rec.mesh_file = obj_path.filename().string();
        }

        for (auto& [slot, tex] : capture_textures(dev, draw_id, frame_id)) {
            std::filesystem::path tex_path = g_output_dir / (tex.name + ".png");
            bool written = exporters::write_png(tex, tex_path);
            if (!written) {
                tex_path = g_output_dir / (tex.name + ".dds");
                written = exporters::write_dds(tex, tex_path);
            }
            if (written) {
                exporters::DrawMaterialRecord::Tex t;
                t.slot          = static_cast<std::uint32_t>(slot);
                t.file          = tex_path.filename().string();
                t.native_format = tex.native_format;
                t.width         = tex.width;
                t.height        = tex.height;
                t.mips          = tex.mip_levels;
                rec.ps_textures.push_back(std::move(t));
            }
        }
        g_pending_materials.push_back(std::move(rec));
    } catch (...) {
        OR_LOG_WARN("capture: exception in draw {} - skipping", draw_id);
    }
}

void try_capture_up(D3DPRIMITIVETYPE prim_type, UINT prim_count,
                    const void* vdata, UINT vstride,
                    const void* idata, D3DFORMAT ifmt,
                    UINT /*min_vertex*/, UINT num_vertices)
{
    const auto draw_id  = g_capture_draw_idx.fetch_add(1, std::memory_order_relaxed);
    const auto frame_id = static_cast<std::uint32_t>(
                              g_frame_counter.load(std::memory_order_relaxed));
    try {
        exporters::DrawMaterialRecord rec;
        rec.draw_id = draw_id;

        auto mesh = capture_mesh_up(prim_type, prim_count, vdata, vstride,
                                    idata, ifmt, num_vertices, draw_id, frame_id);
        if (mesh) {
            const auto obj_path = g_output_dir / (mesh->name + ".obj");
            if (exporters::write_obj(*mesh, obj_path, g_flip_winding))
                rec.mesh_file = obj_path.filename().string();
        }
        g_pending_materials.push_back(std::move(rec));
    } catch (...) {
        OR_LOG_WARN("capture: exception in draw {} - skipping", draw_id);
    }
}

// ---- flush_frame -------------------------------------------------------------
std::uint32_t flush_frame(std::uint64_t frame) {
    const auto draw_count = static_cast<std::uint32_t>(g_pending_materials.size());
    if (!g_pending_materials.empty()) {
        const auto json_path = g_output_dir /
            std::format("frame{:06}_materials.json", frame);
        exporters::write_material_manifest(
            static_cast<std::uint32_t>(frame), g_pending_materials, json_path);
        g_session_frames.push_back({
            static_cast<std::uint32_t>(frame), draw_count,
            json_path.filename().string()});
        g_pending_materials.clear();
    }
    OR_LOG_INFO("capture: frame {:06} complete ({} draws written)", frame, draw_count);
    return draw_count;
}

// ---- Hook bodies -------------------------------------------------------------

HRESULT STDMETHODCALLTYPE hooked_present(
    IDirect3DDevice9* dev, CONST RECT* src, CONST RECT* dst,
    HWND hwnd, CONST RGNDATA* dirty)
{
    const auto frame  = g_frame_counter.fetch_add(1, std::memory_order_relaxed);
    const auto draws  = g_draws_this_frame.exchange(0, std::memory_order_relaxed);
    const auto target = g_capture_frame_target.load(std::memory_order_relaxed);

    if ((frame % 60) == 0)
        OR_LOG_DEBUG("frame {} - {} draw calls", frame, draws);

    // 1. End-of-frame flush + decrement
    if (g_capture_active.load(std::memory_order_relaxed)) {
        const auto draw_count = flush_frame(frame);
        const auto rem = g_freeze_frames_remaining.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (rem == 0) {
            g_capture_active.store(false, std::memory_order_release);
            overlay_notify(frame, draw_count);
        } else {
            g_capture_draw_idx.store(0, std::memory_order_relaxed);
            OR_LOG_INFO("capture: {} more frame(s) to go", rem);
        }
    }

    // 2. Arm: config trigger
    if (target != std::numeric_limits<std::uint64_t>::max() &&
        frame + 1 == target &&
        !g_capture_active.load(std::memory_order_relaxed))
    {
        g_freeze_frames_remaining.store(g_freeze_count, std::memory_order_release);
        g_capture_draw_idx.store(0, std::memory_order_relaxed);
        g_pending_materials.clear();
        g_capture_active.store(true, std::memory_order_release);
        OR_LOG_INFO("capture: frame {:06} begin - capturing {} frame(s)",
                    target, g_freeze_count);
    }

    // 3. Arm: hotkey trigger
    if (g_freeze_frames_remaining.load(std::memory_order_acquire) > 0 &&
        !g_capture_active.load(std::memory_order_relaxed))
    {
        g_capture_draw_idx.store(0, std::memory_order_relaxed);
        g_pending_materials.clear();
        g_capture_active.store(true, std::memory_order_release);
        OR_LOG_INFO("hotkey: frame {:06} begin - capturing {} frame(s)",
                    frame + 1, g_freeze_frames_remaining.load(std::memory_order_relaxed));
    }

    // 4. Overlay (window title)
    if (hwnd) overlay_draw(hwnd, frame);

    // time_freeze_on_rip: suppress the real Present while capture is active
    if (g_capture_active.load(std::memory_order_relaxed) && g_time_freeze_on_rip)
        return D3D_OK;

    return g_real_present(dev, src, dst, hwnd, dirty);
}

HRESULT STDMETHODCALLTYPE hooked_draw_primitive(
    IDirect3DDevice9* dev, D3DPRIMITIVETYPE prim_type,
    UINT start_vertex, UINT prim_count)
{
    const HRESULT hr = g_real_draw_primitive(dev, prim_type, start_vertex, prim_count);
    g_draws_this_frame.fetch_add(1, std::memory_order_relaxed);
    if (g_capture_active.load(std::memory_order_acquire))
        try_capture_nonindexed(dev, prim_type, start_vertex, prim_count);
    return hr;
}

HRESULT STDMETHODCALLTYPE hooked_draw_indexed_primitive(
    IDirect3DDevice9* dev, D3DPRIMITIVETYPE prim_type,
    INT base_vertex, UINT min_vertex, UINT num_vertices,
    UINT start_index, UINT prim_count)
{
    const HRESULT hr = g_real_draw_indexed(dev, prim_type, base_vertex, min_vertex,
                                            num_vertices, start_index, prim_count);
    g_draws_this_frame.fetch_add(1, std::memory_order_relaxed);
    if (g_capture_active.load(std::memory_order_acquire))
        try_capture_indexed(dev, prim_type, base_vertex, min_vertex, num_vertices,
                            start_index, prim_count);
    return hr;
}

HRESULT STDMETHODCALLTYPE hooked_draw_primitive_up(
    IDirect3DDevice9* dev, D3DPRIMITIVETYPE prim_type,
    UINT prim_count, CONST void* vdata, UINT vstride)
{
    const HRESULT hr = g_real_draw_primitive_up(dev, prim_type, prim_count, vdata, vstride);
    g_draws_this_frame.fetch_add(1, std::memory_order_relaxed);
    if (g_capture_active.load(std::memory_order_acquire)) {
        UINT num_verts = 0;
        switch (prim_type) {
        case D3DPT_TRIANGLELIST:  num_verts = prim_count * 3; break;
        case D3DPT_TRIANGLESTRIP: num_verts = prim_count + 2; break;
        default: num_verts = prim_count * 2; break;
        }
        try_capture_up(prim_type, prim_count, vdata, vstride,
                       nullptr, D3DFMT_INDEX16, 0, num_verts);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE hooked_draw_indexed_primitive_up(
    IDirect3DDevice9* dev, D3DPRIMITIVETYPE prim_type,
    UINT min_vertex, UINT num_vertices, UINT prim_count,
    CONST void* idata, D3DFORMAT ifmt,
    CONST void* vdata, UINT vstride)
{
    const HRESULT hr = g_real_draw_indexed_up(dev, prim_type, min_vertex, num_vertices,
                                               prim_count, idata, ifmt, vdata, vstride);
    g_draws_this_frame.fetch_add(1, std::memory_order_relaxed);
    if (g_capture_active.load(std::memory_order_acquire))
        try_capture_up(prim_type, prim_count, vdata, vstride,
                       idata, ifmt, min_vertex, num_vertices);
    return hr;
}

// ---- vtable acquisition (dummy NULLREF device) ------------------------------

struct VTableAddrs {
    void* present                    = nullptr;
    void* draw_primitive              = nullptr;
    void* draw_indexed_primitive      = nullptr;
    void* draw_primitive_up           = nullptr;
    void* draw_indexed_primitive_up   = nullptr;
};

HWND create_throwaway_window() {
    static const wchar_t* k_class = L"OpenRipperD9DummyWnd";
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
    return ::CreateWindowExW(0, k_class, L"OR_D9",
                             WS_OVERLAPPEDWINDOW, 0, 0, 1, 1,
                             nullptr, nullptr, ::GetModuleHandleW(nullptr), nullptr);
}

bool acquire_vtable_addrs(VTableAddrs& out) {
    HWND hwnd = create_throwaway_window();
    if (!hwnd) {
        OR_LOG_ERROR("d3d9 vtable-scan: CreateWindowExW failed");
        return false;
    }

    IDirect3D9* d3d = ::Direct3DCreate9(D3D_SDK_VERSION);
    if (!d3d) {
        OR_LOG_ERROR("d3d9 vtable-scan: Direct3DCreate9 failed");
        ::DestroyWindow(hwnd);
        return false;
    }

    D3DPRESENT_PARAMETERS pp{};
    pp.Windowed      = TRUE;
    pp.SwapEffect    = D3DSWAPEFFECT_DISCARD;
    pp.hDeviceWindow = hwnd;

    IDirect3DDevice9* dev = nullptr;
    // Try NULLREF first (no hardware required); fall back to HAL.
    HRESULT hr = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_NULLREF, hwnd,
                                   D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &dev);
    if (FAILED(hr))
        hr = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
                               D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &dev);
    d3d->Release();

    if (FAILED(hr)) {
        OR_LOG_ERROR("d3d9 vtable-scan: CreateDevice failed (hr=0x{:08X})",
                     static_cast<unsigned>(hr));
        ::DestroyWindow(hwnd);
        return false;
    }

    void** vt = *reinterpret_cast<void***>(dev);
    out.present                   = vt[k_vt_present];
    out.draw_primitive             = vt[k_vt_draw_primitive];
    out.draw_indexed_primitive     = vt[k_vt_draw_indexed_primitive];
    out.draw_primitive_up          = vt[k_vt_draw_primitive_up];
    out.draw_indexed_primitive_up  = vt[k_vt_draw_indexed_primitive_up];

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

// ---- Public API --------------------------------------------------------------

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
                          reinterpret_cast<void**>(&g_real_present), "Present");
    ok &= create_one_hook(addrs.draw_primitive,
                          reinterpret_cast<void*>(&hooked_draw_primitive),
                          reinterpret_cast<void**>(&g_real_draw_primitive), "DrawPrimitive");
    ok &= create_one_hook(addrs.draw_indexed_primitive,
                          reinterpret_cast<void*>(&hooked_draw_indexed_primitive),
                          reinterpret_cast<void**>(&g_real_draw_indexed), "DrawIndexedPrimitive");
    ok &= create_one_hook(addrs.draw_primitive_up,
                          reinterpret_cast<void*>(&hooked_draw_primitive_up),
                          reinterpret_cast<void**>(&g_real_draw_primitive_up), "DrawPrimitiveUP");
    ok &= create_one_hook(addrs.draw_indexed_primitive_up,
                          reinterpret_cast<void*>(&hooked_draw_indexed_primitive_up),
                          reinterpret_cast<void**>(&g_real_draw_indexed_up), "DrawIndexedPrimitiveUP");

    if (!ok) { MH_Uninitialize(); return false; }

    if (MH_EnableHook(MH_ALL_HOOKS) != MH_OK) {
        OR_LOG_ERROR("MH_EnableHook(MH_ALL_HOOKS) failed");
        MH_Uninitialize();
        return false;
    }

    g_installed.store(true, std::memory_order_release);
    OR_LOG_INFO("D3D9 hooks installed (Present + DrawPrimitive + DrawIndexedPrimitive + UP variants).");
    return true;
}

void remove_hooks() {
    std::lock_guard lk(g_install_mu);
    if (!g_installed.load(std::memory_order_relaxed)) return;

    openripper::stop_hotkey_thread();

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
    g_pending_materials.clear();
    g_installed.store(false, std::memory_order_release);
    OR_LOG_INFO("D3D9 hooks removed.");
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

} // namespace openripper::backends::d3d9
