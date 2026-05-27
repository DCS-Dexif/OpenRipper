// OpenRipper - src/backends/d3d12/hooks_d3d12.cpp
//
// D3D12 / DXGI capture backend (Stage 5).
//
// HOOKED METHODS (15 total)
//   IDXGISwapChain                  [8]  Present
//   ID3D12GraphicsCommandList       [10] Reset
//   ID3D12GraphicsCommandList       [12] DrawInstanced
//   ID3D12GraphicsCommandList       [13] DrawIndexedInstanced
//   ID3D12GraphicsCommandList       [20] IASetPrimitiveTopology
//   ID3D12GraphicsCommandList       [25] SetPipelineState
//   ID3D12GraphicsCommandList       [28] SetDescriptorHeaps
//   ID3D12GraphicsCommandList       [32] SetGraphicsRootDescriptorTable
//   ID3D12GraphicsCommandList       [43] IASetIndexBuffer
//   ID3D12GraphicsCommandList       [44] IASetVertexBuffers
//   ID3D12CommandQueue              [10] ExecuteCommandLists
//   ID3D12Device                    [10] CreateGraphicsPipelineState
//   ID3D12Device                    [18] CreateShaderResourceView
//   ID3D12Device                    [27] CreateCommittedResource
//   ID3D12Device                    [29] CreatePlacedResource
//
// CAPTURE STATE MACHINE (same as D3D11)
//   1. Draw hooks record draw state; UPLOAD-heap VBs/IBs are read immediately.
//   2. hooked_present:
//      a. End-of-frame: flush pending draws (DEFAULT-heap readback), write
//         OBJ/textures, flush material JSON, decrement freeze counter.
//      b. Arm: config trigger (pre-activate for target frame).
//      c. Arm: hotkey trigger (counter set by hotkey thread).
//      d. time_freeze: skip real Present if capture active.
//      e. Overlay + real Present.

#include "hooks_d3d12.hpp"
#include "state_d3d12.hpp"
#include "capture_d3d12.hpp"
#include "runtime_state.hpp"
#include "overlay_d3d12.hpp"

#include "core/hotkey.hpp"
#include "core/logger.hpp"
#include "exporters/obj_exporter.hpp"
#include "exporters/dds_exporter.hpp"
#include "exporters/png_exporter.hpp"
#include "exporters/material_exporter.hpp"
#include "exporters/session_exporter.hpp"

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <dxgi.h>
#include <MinHook.h>

#include <atomic>
#include <cstddef>
#include <filesystem>
#include <format>
#include <limits>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace openripper::backends::d3d12 {

// ---- Device / queue pointers captured from hooks --------------------------
ID3D12Device*       g_device     = nullptr;
ID3D12CommandQueue* g_game_queue = nullptr;

namespace {

// ---- vtable indices -------------------------------------------------------
constexpr std::size_t k_vt_present           = 8;   // IDXGISwapChain
// ID3D12GraphicsCommandList:
constexpr std::size_t k_vt_cl_reset          = 10;
constexpr std::size_t k_vt_cl_draw           = 12;
constexpr std::size_t k_vt_cl_draw_indexed   = 13;
constexpr std::size_t k_vt_cl_ia_topology    = 20;
constexpr std::size_t k_vt_cl_set_pso        = 25;
constexpr std::size_t k_vt_cl_set_heaps      = 28;
constexpr std::size_t k_vt_cl_set_gfx_root_tbl = 32;
constexpr std::size_t k_vt_cl_ia_ib          = 43;
constexpr std::size_t k_vt_cl_ia_vbs         = 44;
// ID3D12CommandQueue:
constexpr std::size_t k_vt_q_execute_cls     = 10;
// ID3D12Device:
constexpr std::size_t k_vt_dev_create_gfx_pso= 10;
constexpr std::size_t k_vt_dev_create_srv    = 18;
constexpr std::size_t k_vt_dev_create_comres = 27;
constexpr std::size_t k_vt_dev_create_placed = 29;

// ---- Function pointer types -----------------------------------------------
using Present_t      = HRESULT (STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using ClReset_t      = HRESULT (STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12CommandAllocator*, ID3D12PipelineState*);
using DrawInst_t     = void    (STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, UINT, UINT, UINT);
using DrawIdxInst_t  = void    (STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, UINT, UINT, INT, UINT);
using IATopo_t       = void    (STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, D3D12_PRIMITIVE_TOPOLOGY);
using SetPso_t       = void    (STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12PipelineState*);
using SetHeaps_t     = void    (STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, ID3D12DescriptorHeap* const*);
using SetGfxRootTbl_t= void    (STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, D3D12_GPU_DESCRIPTOR_HANDLE);
using IASetIB_t      = void    (STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, const D3D12_INDEX_BUFFER_VIEW*);
using IASetVBs_t     = void    (STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, UINT, const D3D12_VERTEX_BUFFER_VIEW*);
using ExecCLs_t      = void    (STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
using CreateGfxPso_t = HRESULT (STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_GRAPHICS_PIPELINE_STATE_DESC*, REFIID, void**);
using CreateSrv_t    = void    (STDMETHODCALLTYPE*)(ID3D12Device*, ID3D12Resource*, const D3D12_SHADER_RESOURCE_VIEW_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE);
using CreateComRes_t = HRESULT (STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_HEAP_PROPERTIES*, D3D12_HEAP_FLAGS,
                                                     const D3D12_RESOURCE_DESC*, D3D12_RESOURCE_STATES,
                                                     const D3D12_CLEAR_VALUE*, REFIID, void**);
using CreatePlaced_t = HRESULT (STDMETHODCALLTYPE*)(ID3D12Device*, ID3D12Heap*, UINT64,
                                                     const D3D12_RESOURCE_DESC*, D3D12_RESOURCE_STATES,
                                                     const D3D12_CLEAR_VALUE*, REFIID, void**);

Present_t       g_real_present       = nullptr;
ClReset_t       g_real_cl_reset      = nullptr;
DrawInst_t      g_real_draw_inst     = nullptr;
DrawIdxInst_t   g_real_draw_idx_inst = nullptr;
IATopo_t        g_real_ia_topo       = nullptr;
SetPso_t        g_real_set_pso       = nullptr;
SetHeaps_t      g_real_set_heaps     = nullptr;
SetGfxRootTbl_t g_real_set_gfx_tbl  = nullptr;
IASetIB_t       g_real_ia_ib         = nullptr;
IASetVBs_t      g_real_ia_vbs        = nullptr;
ExecCLs_t       g_real_exec_cls      = nullptr;
CreateGfxPso_t  g_real_create_pso    = nullptr;
CreateSrv_t     g_real_create_srv    = nullptr;
CreateComRes_t  g_real_create_comres = nullptr;
CreatePlaced_t  g_real_create_placed = nullptr;

// ---- Per-frame counters ---------------------------------------------------
std::atomic<std::uint64_t> g_frame_counter{0};
std::atomic<std::uint64_t> g_draws_this_frame{0};

// ---- Capture state --------------------------------------------------------
std::atomic<bool>          g_capture_active{false};
std::atomic<std::uint32_t> g_capture_draw_idx{0};
std::vector<exporters::DrawMaterialRecord> g_pending_materials;
std::vector<exporters::FrameRecord>        g_session_frames;

std::atomic<bool> g_installed{false};
std::mutex        g_install_mu;

// ---- try_record -----------------------------------------------------------
// Records a draw call during a capture frame, copying UPLOAD-heap VB/IB data.

void try_record(ID3D12GraphicsCommandList* cl,
                std::uint32_t index_count,
                std::uint32_t vertex_count,
                std::uint32_t start_index,
                std::int32_t  base_vertex)
{
    const auto draw_id  = g_capture_draw_idx.fetch_add(1, std::memory_order_relaxed);
    const auto frame_id = static_cast<std::uint32_t>(g_frame_counter.load(std::memory_order_relaxed));

    DrawRecord rec;
    rec.draw_id      = draw_id;
    rec.frame_id     = frame_id;
    rec.index_count  = index_count;
    rec.vertex_count = vertex_count;
    rec.start_index  = start_index;
    rec.base_vertex  = base_vertex;

    PerClState cls = cl_state_get(cl);
    rec.vb_count  = cls.vb_count;
    rec.ib_view   = cls.ib_view;
    rec.topology  = cls.topology;
    rec.pso       = cls.pso;
    if (rec.pso) rec.pso->AddRef();
    for (UINT i = 0; i < cls.vb_count; ++i)
        rec.vb_views[i] = cls.vb_views[i];

    // Resolve VB resources.
    for (UINT i = 0; i < rec.vb_count; ++i) {
        const auto& vbv = rec.vb_views[i];
        if (!vbv.BufferLocation || !vbv.SizeInBytes) continue;

        auto entry = va_map_lookup(vbv.BufferLocation);
        if (!entry) { OR_LOG_DEBUG("d3d12: VB VA 0x{:X} not in map (draw {})", vbv.BufferLocation, draw_id); continue; }

        entry->resource->AddRef();
        if (entry->heap_type == D3D12_HEAP_TYPE_UPLOAD) {
            // CPU-visible: Map and copy now.
            void* ptr = nullptr;
            D3D12_RANGE range{0, 0}; // read-only map
            if (SUCCEEDED(entry->resource->Map(0, &range, &ptr))) {
                UINT64 offset = vbv.BufferLocation - entry->resource->GetGPUVirtualAddress();
                rec.immediate_vb[i].resize(vbv.SizeInBytes);
                std::memcpy(rec.immediate_vb[i].data(),
                            static_cast<const std::byte*>(ptr) + offset,
                            vbv.SizeInBytes);
                entry->resource->Unmap(0, nullptr);
            }
            entry->resource->Release(); // not needed after copy
        } else {
            // DEFAULT heap: defer to Present.
            rec.vb_resources[i]      = entry->resource; // AddRef'd above
            rec.vb_byte_offsets[i]   = vbv.BufferLocation - entry->resource->GetGPUVirtualAddress();
            rec.vb_byte_sizes[i]     = vbv.SizeInBytes;
        }
    }

    // Resolve IB resource.
    if (rec.ib_view.BufferLocation && rec.ib_view.SizeInBytes) {
        auto entry = va_map_lookup(rec.ib_view.BufferLocation);
        if (entry) {
            entry->resource->AddRef();
            if (entry->heap_type == D3D12_HEAP_TYPE_UPLOAD) {
                void* ptr = nullptr;
                D3D12_RANGE range{0, 0};
                if (SUCCEEDED(entry->resource->Map(0, &range, &ptr))) {
                    UINT64 offset = rec.ib_view.BufferLocation - entry->resource->GetGPUVirtualAddress();
                    rec.immediate_ib.resize(rec.ib_view.SizeInBytes);
                    std::memcpy(rec.immediate_ib.data(),
                                static_cast<const std::byte*>(ptr) + offset,
                                rec.ib_view.SizeInBytes);
                    entry->resource->Unmap(0, nullptr);
                }
                entry->resource->Release();
            } else {
                rec.ib_resource    = entry->resource;
                rec.ib_byte_offset = rec.ib_view.BufferLocation - entry->resource->GetGPUVirtualAddress();
                rec.ib_byte_size   = rec.ib_view.SizeInBytes;
            }
        }
    }

    // Collect textures from SRVs in bound descriptor heaps.
    // Walk all CPU descriptor slots in each bound CBV/SRV/UAV heap and check
    // against the SRV map. Cap at 128 unique textures per frame to match
    // D3D11's 128-slot behaviour.
    static thread_local std::vector<ID3D12Resource*> seen_textures; // dedup within a draw
    seen_textures.clear();
    UINT tex_slot = 0;

    for (UINT h = 0; h < cls.heap_count && tex_slot < 128; ++h) {
        const auto& heap = cls.heaps[h];
        for (UINT s = 0; s < heap.count && tex_slot < 128; ++s) {
            SIZE_T cpu_ptr = heap.cpu_start + static_cast<SIZE_T>(s) * heap.increment;
            auto srv = srv_lookup(cpu_ptr);
            if (!srv) continue;
            if (srv->dimension != D3D12_SRV_DIMENSION_TEXTURE2D) continue;

            // Dedup within this draw.
            bool dup = false;
            for (auto* r : seen_textures) { if (r == srv->resource) { dup = true; break; } }
            if (dup) continue;

            srv->resource->AddRef();
            rec.textures.emplace_back(tex_slot, srv->resource);
            seen_textures.push_back(srv->resource);
            ++tex_slot;
        }
    }

    draw_record_push(std::move(rec));
}

// ---- flush_frame ----------------------------------------------------------
std::uint32_t flush_frame(std::uint64_t frame) {
    auto recs = draw_records_take();
    const auto draw_count = static_cast<std::uint32_t>(recs.size());

    if (!recs.empty()) {
        // Readback DEFAULT-heap resources and textures.
        std::vector<TextureReadback> tex_readbacks;
        if (g_device && g_game_queue)
            capture_readback_defaults(recs, g_device, g_game_queue, tex_readbacks);

        // Produce MeshSnapshots and write OBJ files.
        for (auto& rec : recs) {
            exporters::DrawMaterialRecord mat_rec;
            mat_rec.draw_id = rec.draw_id;

            auto snap = make_mesh_snapshot(rec, rec.draw_id, rec.frame_id);
            if (snap) {
                const auto obj_out = g_output_dir / (snap->name + ".obj");
                if (exporters::write_obj(*snap, obj_out, g_flip_winding))
                    mat_rec.mesh_file = obj_out.filename().string();
            }
            g_pending_materials.push_back(std::move(mat_rec));
            draw_record_release(rec);
        }

        // Assign textures to the corresponding material records.
        // When g_dedup is on, a per-frame map keyed on source_resource avoids
        // writing the same GPU resource more than once per frame.
        struct DedupEntry12 {
            std::string   file;
            std::uint32_t native_format{0}, width{0}, height{0}, mip_levels{0};
        };
        std::unordered_map<ID3D12Resource*, DedupEntry12> tex_dedup;

        for (auto& tr : tex_readbacks) {
            std::string filename;
            std::uint32_t fmt{0}, w{0}, h{0}, mips{0};

            if (g_dedup && tr.source_resource) {
                auto it = tex_dedup.find(tr.source_resource);
                if (it != tex_dedup.end()) {
                    const auto& de = it->second;
                    filename = de.file;
                    fmt = de.native_format; w = de.width; h = de.height; mips = de.mip_levels;
                    OR_LOG_DEBUG("dedup: draw {} slot {} -> reusing {}",
                                 tr.draw_idx, tr.srv_slot, filename);
                }
            }

            if (filename.empty()) {
                // Not a dedup hit — write the file.
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
                    if (g_dedup && tr.source_resource)
                        tex_dedup.insert_or_assign(tr.source_resource,
                            DedupEntry12{filename, fmt, w, h, mips});
                }
            }

            if (!filename.empty()) {
                for (auto& mat : g_pending_materials) {
                    if (mat.draw_id != tr.draw_idx) continue;
                    exporters::DrawMaterialRecord::Tex t;
                    t.slot          = tr.srv_slot;
                    t.file          = filename;
                    t.native_format = fmt;
                    t.width         = w;
                    t.height        = h;
                    t.mips          = mips;
                    mat.ps_textures.push_back(std::move(t));
                    break;
                }
            }
        }

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

    OR_LOG_INFO("d3d12 capture: frame {:06} complete ({} draws)", frame, draw_count);
    return draw_count;
}

// ---- Hook bodies ----------------------------------------------------------

HRESULT STDMETHODCALLTYPE hooked_present(IDXGISwapChain* sc, UINT sync_interval, UINT flags) {
    const auto frame  = g_frame_counter.fetch_add(1, std::memory_order_relaxed);
    const auto draws  = g_draws_this_frame.exchange(0, std::memory_order_relaxed);
    const auto target = g_capture_frame_target.load(std::memory_order_relaxed);

    if ((frame % 60) == 0)
        OR_LOG_DEBUG("d3d12: frame {} - {} draw calls", frame, draws);

    // 1. End-of-frame flush.
    if (g_capture_active.load(std::memory_order_relaxed)) {
        const auto draw_count = flush_frame(frame);
        const auto rem = g_freeze_frames_remaining.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (rem == 0) {
            g_capture_active.store(false, std::memory_order_release);
            overlay_notify(frame, draw_count);
        } else {
            g_capture_draw_idx.store(0, std::memory_order_relaxed);
            OR_LOG_INFO("d3d12 capture: {} more frame(s) to go", rem);
        }
    }

    // 2. Arm: config trigger.
    if (target != std::numeric_limits<std::uint64_t>::max() &&
        frame + 1 == target &&
        !g_capture_active.load(std::memory_order_relaxed))
    {
        g_freeze_frames_remaining.store(g_freeze_count, std::memory_order_release);
        g_capture_draw_idx.store(0, std::memory_order_relaxed);
        g_capture_active.store(true, std::memory_order_release);
        OR_LOG_INFO("d3d12 capture: frame {:06} begin - capturing {} frame(s)", target, g_freeze_count);
    }

    // 3. Arm: hotkey trigger.
    if (g_freeze_frames_remaining.load(std::memory_order_acquire) > 0 &&
        !g_capture_active.load(std::memory_order_relaxed))
    {
        g_capture_draw_idx.store(0, std::memory_order_relaxed);
        g_capture_active.store(true, std::memory_order_release);
        OR_LOG_INFO("d3d12 hotkey: frame {:06} begin - capturing {} frame(s)",
                    frame + 1, g_freeze_frames_remaining.load(std::memory_order_relaxed));
    }

    // 4. Overlay + Present.
    overlay_draw(sc, frame);
    if (g_capture_active.load(std::memory_order_relaxed) && g_time_freeze_on_rip)
        return S_OK;
    return g_real_present(sc, sync_interval, flags);
}

HRESULT STDMETHODCALLTYPE hooked_cl_reset(ID3D12GraphicsCommandList* cl,
                                           ID3D12CommandAllocator* alloc,
                                           ID3D12PipelineState* pso)
{
    HRESULT hr = g_real_cl_reset(cl, alloc, pso);
    if (SUCCEEDED(hr)) cl_state_reset(cl);
    return hr;
}

void STDMETHODCALLTYPE hooked_draw_instanced(ID3D12GraphicsCommandList* cl,
                                              UINT vtx_per_inst, UINT inst_count,
                                              UINT start_vtx, UINT start_inst)
{
    g_real_draw_inst(cl, vtx_per_inst, inst_count, start_vtx, start_inst);
    g_draws_this_frame.fetch_add(1, std::memory_order_relaxed);
    if (g_capture_active.load(std::memory_order_acquire))
        try_record(cl, 0, vtx_per_inst, 0, static_cast<std::int32_t>(start_vtx));
}

void STDMETHODCALLTYPE hooked_draw_indexed_instanced(ID3D12GraphicsCommandList* cl,
                                                      UINT idx_per_inst, UINT inst_count,
                                                      UINT start_idx, INT base_vtx,
                                                      UINT start_inst)
{
    g_real_draw_idx_inst(cl, idx_per_inst, inst_count, start_idx, base_vtx, start_inst);
    g_draws_this_frame.fetch_add(1, std::memory_order_relaxed);
    if (g_capture_active.load(std::memory_order_acquire))
        try_record(cl, idx_per_inst, 0, start_idx, base_vtx);
}

void STDMETHODCALLTYPE hooked_ia_set_topology(ID3D12GraphicsCommandList* cl,
                                               D3D12_PRIMITIVE_TOPOLOGY topo)
{
    g_real_ia_topo(cl, topo);
    PerClState s = cl_state_get(cl);
    s.topology = topo;
    cl_state_set(cl, std::move(s));
}

void STDMETHODCALLTYPE hooked_set_pso(ID3D12GraphicsCommandList* cl,
                                       ID3D12PipelineState* pso)
{
    g_real_set_pso(cl, pso);
    PerClState s = cl_state_get(cl);
    s.pso = pso;
    cl_state_set(cl, std::move(s));
}

void STDMETHODCALLTYPE hooked_set_descriptor_heaps(ID3D12GraphicsCommandList* cl,
                                                     UINT count,
                                                     ID3D12DescriptorHeap* const* heaps)
{
    g_real_set_heaps(cl, count, heaps);
    PerClState s = cl_state_get(cl);
    s.heap_count = 0;
    for (UINT i = 0; i < count && i < 2; ++i) {
        if (!heaps[i]) continue;
        D3D12_DESCRIPTOR_HEAP_DESC desc = heaps[i]->GetDesc();
        if (desc.Type != D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV) continue;
        HeapBind& hb  = s.heaps[s.heap_count++];
        hb.cpu_start  = heaps[i]->GetCPUDescriptorHandleForHeapStart().ptr;
        hb.gpu_start  = heaps[i]->GetGPUDescriptorHandleForHeapStart().ptr;
        hb.count      = desc.NumDescriptors;
        if (g_device)
            hb.increment = g_device->GetDescriptorHandleIncrementSize(
                               D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        else
            hb.increment = 32; // safe fallback
    }
    cl_state_set(cl, std::move(s));
}

void STDMETHODCALLTYPE hooked_set_gfx_root_table(ID3D12GraphicsCommandList* cl,
                                                   UINT root_param_idx,
                                                   D3D12_GPU_DESCRIPTOR_HANDLE base)
{
    g_real_set_gfx_tbl(cl, root_param_idx, base);
    PerClState s = cl_state_get(cl);
    s.root_tables[root_param_idx] = base.ptr;
    cl_state_set(cl, std::move(s));
}

void STDMETHODCALLTYPE hooked_ia_set_ib(ID3D12GraphicsCommandList* cl,
                                         const D3D12_INDEX_BUFFER_VIEW* view)
{
    g_real_ia_ib(cl, view);
    PerClState s = cl_state_get(cl);
    s.ib_view = view ? *view : D3D12_INDEX_BUFFER_VIEW{};
    cl_state_set(cl, std::move(s));
}

void STDMETHODCALLTYPE hooked_ia_set_vbs(ID3D12GraphicsCommandList* cl,
                                          UINT start_slot, UINT count,
                                          const D3D12_VERTEX_BUFFER_VIEW* views)
{
    g_real_ia_vbs(cl, start_slot, count, views);
    PerClState s = cl_state_get(cl);
    for (UINT i = 0; i < count && start_slot + i < k_max_vb_slots; ++i)
        s.vb_views[start_slot + i] = views ? views[i] : D3D12_VERTEX_BUFFER_VIEW{};
    if (start_slot + count > s.vb_count)
        s.vb_count = start_slot + count;
    cl_state_set(cl, std::move(s));
}

void STDMETHODCALLTYPE hooked_execute_cls(ID3D12CommandQueue* queue,
                                           UINT count,
                                           ID3D12CommandList* const* lists)
{
    g_real_exec_cls(queue, count, lists);

    // Capture the direct command queue pointer (first direct queue seen).
    if (!g_game_queue) {
        D3D12_COMMAND_QUEUE_DESC qd = queue->GetDesc();
        if (qd.Type == D3D12_COMMAND_LIST_TYPE_DIRECT) {
            g_game_queue = queue;
            g_game_queue->AddRef();
            OR_LOG_INFO("d3d12: captured game command queue {:p}",
                        static_cast<void*>(g_game_queue));
        }
    }

    // Ensure readback infra is ready.
    if (g_device && g_game_queue && queue == g_game_queue)
        init_readback_infra(g_device);
}

HRESULT STDMETHODCALLTYPE hooked_create_gfx_pso(ID3D12Device* dev,
                                                  const D3D12_GRAPHICS_PIPELINE_STATE_DESC* desc,
                                                  REFIID riid, void** pp)
{
    HRESULT hr = g_real_create_pso(dev, desc, riid, pp);
    if (SUCCEEDED(hr) && pp && *pp && desc && desc->InputLayout.NumElements > 0) {
        auto* pso = static_cast<ID3D12PipelineState*>(*pp);
        pso_register(pso, desc->InputLayout.pInputElementDescs, desc->InputLayout.NumElements);
    }
    if (!g_device && SUCCEEDED(hr)) { g_device = dev; g_device->AddRef(); }
    return hr;
}

void STDMETHODCALLTYPE hooked_create_srv(ID3D12Device* dev,
                                          ID3D12Resource* resource,
                                          const D3D12_SHADER_RESOURCE_VIEW_DESC* srv_desc,
                                          D3D12_CPU_DESCRIPTOR_HANDLE dest_handle)
{
    g_real_create_srv(dev, resource, srv_desc, dest_handle);

    if (!resource || !srv_desc) return;
    if (srv_desc->ViewDimension != D3D12_SRV_DIMENSION_TEXTURE2D &&
        srv_desc->ViewDimension != D3D12_SRV_DIMENSION_TEXTURE2DARRAY) return;

    const D3D12_RESOURCE_DESC rd = resource->GetDesc();
    if (rd.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D) return;

    SrvEntry entry;
    entry.resource   = resource;
    entry.format     = srv_desc->Format != DXGI_FORMAT_UNKNOWN ? srv_desc->Format : rd.Format;
    entry.dimension  = srv_desc->ViewDimension;
    entry.mip_levels = rd.MipLevels;
    entry.width      = static_cast<UINT>(rd.Width);
    entry.height     = rd.Height;
    srv_insert(dest_handle.ptr, std::move(entry));
}

HRESULT STDMETHODCALLTYPE hooked_create_committed(ID3D12Device* dev,
                                                    const D3D12_HEAP_PROPERTIES* heap_props,
                                                    D3D12_HEAP_FLAGS heap_flags,
                                                    const D3D12_RESOURCE_DESC* res_desc,
                                                    D3D12_RESOURCE_STATES initial_state,
                                                    const D3D12_CLEAR_VALUE* clear_val,
                                                    REFIID riid, void** pp)
{
    HRESULT hr = g_real_create_comres(dev, heap_props, heap_flags, res_desc,
                                       initial_state, clear_val, riid, pp);
    if (SUCCEEDED(hr) && pp && *pp && res_desc &&
        res_desc->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
    {
        auto* res = static_cast<ID3D12Resource*>(*pp);
        const UINT64 va = res->GetGPUVirtualAddress();
        if (va)
            va_map_insert(va, res, res_desc->Width, heap_props->Type);
    }
    if (!g_device && SUCCEEDED(hr)) { g_device = dev; g_device->AddRef(); }
    return hr;
}

HRESULT STDMETHODCALLTYPE hooked_create_placed(ID3D12Device* dev,
                                                ID3D12Heap* heap, UINT64 heap_offset,
                                                const D3D12_RESOURCE_DESC* res_desc,
                                                D3D12_RESOURCE_STATES initial_state,
                                                const D3D12_CLEAR_VALUE* clear_val,
                                                REFIID riid, void** pp)
{
    HRESULT hr = g_real_create_placed(dev, heap, heap_offset, res_desc,
                                       initial_state, clear_val, riid, pp);
    if (SUCCEEDED(hr) && pp && *pp && res_desc &&
        res_desc->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
    {
        auto* res = static_cast<ID3D12Resource*>(*pp);
        const UINT64 va = res->GetGPUVirtualAddress();
        if (va) {
            const D3D12_HEAP_DESC hd = heap->GetDesc();
            va_map_insert(va, res, res_desc->Width, hd.Properties.Type);
        }
    }
    if (!g_device && SUCCEEDED(hr)) { g_device = dev; g_device->AddRef(); }
    return hr;
}

// ---- vtable acquisition ---------------------------------------------------

struct VTableAddrs {
    void* present             = nullptr;
    void* cl_reset            = nullptr;
    void* draw_inst           = nullptr;
    void* draw_idx_inst       = nullptr;
    void* ia_topo             = nullptr;
    void* set_pso             = nullptr;
    void* set_heaps           = nullptr;
    void* set_gfx_root_tbl    = nullptr;
    void* ia_ib               = nullptr;
    void* ia_vbs              = nullptr;
    void* exec_cls            = nullptr;
    void* create_pso          = nullptr;
    void* create_srv          = nullptr;
    void* create_comres       = nullptr;
    void* create_placed       = nullptr;
};

HWND create_dummy_window_d3d12() {
    static const wchar_t* k_class = L"OpenRipperDummyWnd12";
    static const bool reg = []() {
        WNDCLASSEXW wc{};
        wc.cbSize        = sizeof(wc);
        wc.lpfnWndProc   = ::DefWindowProcW;
        wc.hInstance     = ::GetModuleHandleW(nullptr);
        wc.lpszClassName = k_class;
        ::RegisterClassExW(&wc);
        return true;
    }();
    (void)reg;
    return ::CreateWindowExW(0, k_class, L"OpenRipperDummy12",
                             WS_OVERLAPPEDWINDOW, 0, 0, 1, 1,
                             nullptr, nullptr, ::GetModuleHandleW(nullptr), nullptr);
}

bool acquire_vtable_addrs(VTableAddrs& out) {
    HWND hwnd = create_dummy_window_d3d12();
    if (!hwnd) {
        OR_LOG_ERROR("d3d12 vtable-scan: CreateWindowExW failed (gle={})", ::GetLastError());
        return false;
    }

    ID3D12Device* dev = nullptr;
    HRESULT hr = ::D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0,
                                      IID_PPV_ARGS(&dev));
    if (FAILED(hr)) {
        OR_LOG_ERROR("d3d12 vtable-scan: D3D12CreateDevice failed (hr=0x{:08X})", static_cast<unsigned>(hr));
        ::DestroyWindow(hwnd); return false;
    }

    D3D12_COMMAND_QUEUE_DESC qd{D3D12_COMMAND_LIST_TYPE_DIRECT, 0,
                                 D3D12_COMMAND_QUEUE_FLAG_NONE, 0};
    ID3D12CommandQueue* q = nullptr;
    if (FAILED(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&q)))) {
        OR_LOG_ERROR("d3d12 vtable-scan: CreateCommandQueue failed");
        dev->Release(); ::DestroyWindow(hwnd); return false;
    }

    ID3D12CommandAllocator* alloc = nullptr;
    dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc));

    ID3D12GraphicsCommandList* cl = nullptr;
    dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc,
                            nullptr, IID_PPV_ARGS(&cl));

    // Swap chain for Present vtable.
    IDXGIFactory4* factory = nullptr;
    ::CreateDXGIFactory2(0, IID_PPV_ARGS(&factory));

    DXGI_SWAP_CHAIN_DESC1 scd{};
    scd.Width = scd.Height = 1;
    scd.Format      = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.SampleDesc.Count = 1;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.BufferCount = 2;
    scd.SwapEffect  = DXGI_SWAP_EFFECT_FLIP_DISCARD;

    IDXGISwapChain1* sc1 = nullptr;
    IDXGISwapChain* sc   = nullptr;
    if (factory) {
        factory->CreateSwapChainForHwnd(q, hwnd, &scd, nullptr, nullptr, &sc1);
        if (sc1) { sc1->QueryInterface(IID_PPV_ARGS(&sc)); sc1->Release(); }
        factory->Release();
    }

    // Read vtable pointers.
    void** vt_dev = *reinterpret_cast<void***>(dev);
    void** vt_q   = *reinterpret_cast<void***>(q);
    void** vt_cl  = cl  ? *reinterpret_cast<void***>(cl)  : nullptr;
    void** vt_sc  = sc  ? *reinterpret_cast<void***>(sc)  : nullptr;

    if (vt_sc)  out.present          = vt_sc [k_vt_present];
    if (vt_cl) {
        out.cl_reset         = vt_cl[k_vt_cl_reset];
        out.draw_inst        = vt_cl[k_vt_cl_draw];
        out.draw_idx_inst    = vt_cl[k_vt_cl_draw_indexed];
        out.ia_topo          = vt_cl[k_vt_cl_ia_topology];
        out.set_pso          = vt_cl[k_vt_cl_set_pso];
        out.set_heaps        = vt_cl[k_vt_cl_set_heaps];
        out.set_gfx_root_tbl = vt_cl[k_vt_cl_set_gfx_root_tbl];
        out.ia_ib            = vt_cl[k_vt_cl_ia_ib];
        out.ia_vbs           = vt_cl[k_vt_cl_ia_vbs];
    }
    out.exec_cls        = vt_q [k_vt_q_execute_cls];
    out.create_pso      = vt_dev[k_vt_dev_create_gfx_pso];
    out.create_srv      = vt_dev[k_vt_dev_create_srv];
    out.create_comres   = vt_dev[k_vt_dev_create_comres];
    out.create_placed   = vt_dev[k_vt_dev_create_placed];

    if (sc)    sc->Release();
    if (cl)    cl->Release();
    if (alloc) alloc->Release();
    q->Release();
    dev->Release();
    ::DestroyWindow(hwnd);
    return out.present && out.draw_inst && out.create_comres;
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

// ---- Public API -----------------------------------------------------------

void activate_capture_if_frame_zero() {
    if (g_capture_frame_target.load(std::memory_order_relaxed) == 0) {
        g_freeze_frames_remaining.store(g_freeze_count, std::memory_order_release);
        g_capture_draw_idx.store(0, std::memory_order_relaxed);
        g_capture_active.store(true, std::memory_order_release);
        OR_LOG_INFO("d3d12 capture: frame 0 targeted - capturing from first draw");
    }
}

bool install_hooks() {
    std::lock_guard lk(g_install_mu);
    if (g_installed.load(std::memory_order_relaxed)) return true;

    if (MH_Initialize() != MH_OK) {
        OR_LOG_ERROR("MinHook initialization failed (d3d12)");
        return false;
    }

    VTableAddrs addrs{};
    if (!acquire_vtable_addrs(addrs)) { MH_Uninitialize(); return false; }

    bool ok = true;
    if (addrs.present)
        ok &= create_one_hook(addrs.present,    reinterpret_cast<void*>(&hooked_present),
                              reinterpret_cast<void**>(&g_real_present),    "Present");
    if (addrs.cl_reset)
        ok &= create_one_hook(addrs.cl_reset,   reinterpret_cast<void*>(&hooked_cl_reset),
                              reinterpret_cast<void**>(&g_real_cl_reset),   "CL::Reset");
    if (addrs.draw_inst)
        ok &= create_one_hook(addrs.draw_inst,  reinterpret_cast<void*>(&hooked_draw_instanced),
                              reinterpret_cast<void**>(&g_real_draw_inst),  "DrawInstanced");
    if (addrs.draw_idx_inst)
        ok &= create_one_hook(addrs.draw_idx_inst, reinterpret_cast<void*>(&hooked_draw_indexed_instanced),
                              reinterpret_cast<void**>(&g_real_draw_idx_inst), "DrawIndexedInstanced");
    if (addrs.ia_topo)
        ok &= create_one_hook(addrs.ia_topo,    reinterpret_cast<void*>(&hooked_ia_set_topology),
                              reinterpret_cast<void**>(&g_real_ia_topo),    "IASetPrimitiveTopology");
    if (addrs.set_pso)
        ok &= create_one_hook(addrs.set_pso,    reinterpret_cast<void*>(&hooked_set_pso),
                              reinterpret_cast<void**>(&g_real_set_pso),    "SetPipelineState");
    if (addrs.set_heaps)
        ok &= create_one_hook(addrs.set_heaps,  reinterpret_cast<void*>(&hooked_set_descriptor_heaps),
                              reinterpret_cast<void**>(&g_real_set_heaps),  "SetDescriptorHeaps");
    if (addrs.set_gfx_root_tbl)
        ok &= create_one_hook(addrs.set_gfx_root_tbl, reinterpret_cast<void*>(&hooked_set_gfx_root_table),
                              reinterpret_cast<void**>(&g_real_set_gfx_tbl), "SetGraphicsRootDescriptorTable");
    if (addrs.ia_ib)
        ok &= create_one_hook(addrs.ia_ib,      reinterpret_cast<void*>(&hooked_ia_set_ib),
                              reinterpret_cast<void**>(&g_real_ia_ib),      "IASetIndexBuffer");
    if (addrs.ia_vbs)
        ok &= create_one_hook(addrs.ia_vbs,     reinterpret_cast<void*>(&hooked_ia_set_vbs),
                              reinterpret_cast<void**>(&g_real_ia_vbs),     "IASetVertexBuffers");
    if (addrs.exec_cls)
        ok &= create_one_hook(addrs.exec_cls,   reinterpret_cast<void*>(&hooked_execute_cls),
                              reinterpret_cast<void**>(&g_real_exec_cls),   "ExecuteCommandLists");
    if (addrs.create_pso)
        ok &= create_one_hook(addrs.create_pso, reinterpret_cast<void*>(&hooked_create_gfx_pso),
                              reinterpret_cast<void**>(&g_real_create_pso), "CreateGraphicsPipelineState");
    if (addrs.create_srv)
        ok &= create_one_hook(addrs.create_srv, reinterpret_cast<void*>(&hooked_create_srv),
                              reinterpret_cast<void**>(&g_real_create_srv), "CreateShaderResourceView");
    if (addrs.create_comres)
        ok &= create_one_hook(addrs.create_comres, reinterpret_cast<void*>(&hooked_create_committed),
                              reinterpret_cast<void**>(&g_real_create_comres), "CreateCommittedResource");
    if (addrs.create_placed)
        ok &= create_one_hook(addrs.create_placed, reinterpret_cast<void*>(&hooked_create_placed),
                              reinterpret_cast<void**>(&g_real_create_placed), "CreatePlacedResource");

    if (!ok) { MH_Uninitialize(); return false; }
    if (MH_EnableHook(MH_ALL_HOOKS) != MH_OK) {
        OR_LOG_ERROR("MH_EnableHook(MH_ALL_HOOKS) failed (d3d12)");
        MH_Uninitialize(); return false;
    }

    g_installed.store(true, std::memory_order_release);
    OR_LOG_INFO("D3D12 hooks installed ({} hooks).", 15);
    return true;
}

void remove_hooks() {
    std::lock_guard lk(g_install_mu);
    if (!g_installed.load(std::memory_order_relaxed)) return;

    openripper::stop_hotkey_thread();
    overlay_shutdown();
    shutdown_readback_infra();

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
    va_map_clear();
    pso_forget_all();
    srv_clear();
    g_pending_materials.clear();
    if (g_device)     { g_device->Release();     g_device     = nullptr; }
    if (g_game_queue) { g_game_queue->Release();  g_game_queue = nullptr; }
    g_installed.store(false, std::memory_order_release);
    OR_LOG_INFO("D3D12 hooks removed.");
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

} // namespace openripper::backends::d3d12
