// OpenRipper - src/backends/d3d12/state_d3d12.hpp
//
// Thread-safe registries shared between hooks_d3d12 and capture_d3d12:
//   - GPU VA map:         gpu_va → (ID3D12Resource*, size, heap_type)
//   - PSO layout registry: ID3D12PipelineState* → vertex input element desc
//   - SRV descriptor map:  CPU handle ptr → texture resource info
//   - Per-command-list state: VB/IB views, topology, PSO, bound heaps,
//                             root descriptor tables
//   - Pending draw records: accumulated during capture frames

#pragma once

#include "../../core/types.hpp"

#include <d3d12.h>
#include <dxgi.h>

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace openripper::backends::d3d12 {

// ---- GPU VA map -----------------------------------------------------------
// Tracks every committed/placed buffer resource so draw hooks can resolve
// D3D12_VERTEX_BUFFER_VIEW::BufferLocation back to the owning ID3D12Resource.

struct VaMapEntry {
    ID3D12Resource*   resource;    // NOT AddRef'd here; AddRef'd at draw time
    UINT64            size;
    D3D12_HEAP_TYPE   heap_type;   // UPLOAD, DEFAULT, or READBACK
};

void   va_map_insert(UINT64 gpu_va, ID3D12Resource* res, UINT64 size, D3D12_HEAP_TYPE type);
void   va_map_remove(UINT64 gpu_va);
// Returns a copy of the entry so the caller can AddRef the resource safely.
std::optional<VaMapEntry> va_map_lookup(UINT64 gpu_va);
void   va_map_clear();

// ---- PSO layout registry --------------------------------------------------
// Mirrors D3D11's input layout registry.  Hooked CreateGraphicsPipelineState
// deep-copies the D3D12_INPUT_ELEMENT_DESC[] for each PSO.

struct PsoLayoutDesc {
    std::vector<D3D12_INPUT_ELEMENT_DESC> elements;
    std::vector<std::string>              name_storage; // owns SemanticName strings
};

void   pso_register(ID3D12PipelineState* pso,
                    const D3D12_INPUT_ELEMENT_DESC* elems, UINT count);
void   pso_forget(ID3D12PipelineState* pso);
void   pso_forget_all();
std::optional<PsoLayoutDesc> pso_lookup(ID3D12PipelineState* pso);

// ---- SRV descriptor map ---------------------------------------------------
// Hooked CreateShaderResourceView records: CPU-handle-ptr → resource info.
// At draw time, descriptor heaps known to be bound are walked to find textures.

struct SrvEntry {
    ID3D12Resource*         resource;     // NOT AddRef'd here
    DXGI_FORMAT             format;
    D3D12_SRV_DIMENSION     dimension;
    UINT                    mip_levels;
    UINT                    width;
    UINT                    height;
};

void   srv_insert(SIZE_T cpu_handle_ptr, SrvEntry entry);
void   srv_clear();
std::optional<SrvEntry> srv_lookup(SIZE_T cpu_handle_ptr);

// ---- Bound heap info (per command list) -----------------------------------
// Populated by the SetDescriptorHeaps hook.

struct HeapBind {
    SIZE_T cpu_start;
    UINT64 gpu_start;
    UINT   increment;
    UINT   count;
};

// ---- Per-command-list state -----------------------------------------------
// Each command list records its own IA / PSO / descriptor state so that when
// a draw fires we know exactly what was bound. D3D12 guarantees single-threaded
// recording per CL, so the inner state struct needs no lock.

constexpr UINT k_max_vb_slots = 16;

struct PerClState {
    D3D12_VERTEX_BUFFER_VIEW    vb_views[k_max_vb_slots]{};
    UINT                        vb_count{0};
    D3D12_INDEX_BUFFER_VIEW     ib_view{};
    D3D_PRIMITIVE_TOPOLOGY      topology{D3D_PRIMITIVE_TOPOLOGY_UNDEFINED};
    ID3D12PipelineState*        pso{nullptr};       // not AddRef'd (PSO registry owns it)

    // Descriptor heaps bound via SetDescriptorHeaps.
    HeapBind                    heaps[2]{};          // [0]=cbv_srv_uav, [1]=sampler
    UINT                        heap_count{0};

    // Root descriptor table GPU base handles, keyed by root param index.
    std::unordered_map<UINT, UINT64> root_tables;   // GPU handle ptr values
};

void cl_state_set(ID3D12GraphicsCommandList* cl, PerClState state);
void cl_state_reset(ID3D12GraphicsCommandList* cl);  // called from Reset hook
PerClState cl_state_get(ID3D12GraphicsCommandList* cl); // returns copy-by-value

// ---- Draw record ----------------------------------------------------------
// One record per draw call during a capture frame.

struct DrawRecord {
    std::uint32_t                  index_count{0};
    std::uint32_t                  vertex_count{0};
    std::uint32_t                  start_index{0};
    std::int32_t                   base_vertex{0};
    D3D12_VERTEX_BUFFER_VIEW       vb_views[k_max_vb_slots]{};
    UINT                           vb_count{0};
    D3D12_INDEX_BUFFER_VIEW        ib_view{};
    D3D_PRIMITIVE_TOPOLOGY         topology{D3D_PRIMITIVE_TOPOLOGY_UNDEFINED};
    ID3D12PipelineState*           pso{nullptr};   // AddRef'd

    // Immediate copies (UPLOAD heap — CPU-copied at draw time).
    std::vector<std::byte>         immediate_vb[k_max_vb_slots];
    std::vector<std::byte>         immediate_ib;

    // Deferred readback resources (DEFAULT heap — copied at Present time).
    ID3D12Resource*                vb_resources[k_max_vb_slots]{};  // AddRef'd
    UINT64                         vb_byte_offsets[k_max_vb_slots]{};
    UINT64                         vb_byte_sizes[k_max_vb_slots]{};
    ID3D12Resource*                ib_resource{nullptr};              // AddRef'd
    UINT64                         ib_byte_offset{0};
    UINT64                         ib_byte_size{0};

    // Textures collected from bound descriptor heaps.
    // Each entry: (slot_index, resource AddRef'd).
    std::vector<std::pair<UINT, ID3D12Resource*>> textures;

    std::uint32_t                  draw_id{0};
    std::uint32_t                  frame_id{0};
};

// Append a draw record (called from draw hooks under the captures mutex).
void draw_record_push(DrawRecord rec);

// Steal all pending records (called from Present hook).
std::vector<DrawRecord> draw_records_take();

// Release any AddRef'd resources in a draw record.
void draw_record_release(DrawRecord& rec);

} // namespace openripper::backends::d3d12
