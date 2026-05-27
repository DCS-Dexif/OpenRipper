// OpenRipper - src/backends/d3d12/capture_d3d12.cpp
//
// GPU->CPU readback and MeshSnapshot construction for D3D12.

#include "capture_d3d12.hpp"
#include "state_d3d12.hpp"
#include "../../core/logger.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <format>
#include <string>

namespace openripper::backends::d3d12 {
namespace {

// ---- Readback infrastructure ----------------------------------------------

ID3D12CommandAllocator*    g_rb_alloc  = nullptr;
ID3D12GraphicsCommandList* g_rb_cl     = nullptr;
ID3D12Fence*               g_rb_fence  = nullptr;
HANDLE                     g_rb_event  = nullptr;
UINT64                     g_rb_fence_val = 0;

// ---- AttributeFormat helpers (mirrors D3D11 capture) ----------------------

using AF = openripper::AttributeFormat;

AF dxgi_to_attr_format(DXGI_FORMAT f) noexcept {
    switch (f) {
    case DXGI_FORMAT_R32_FLOAT:              return AF::Float32x1;
    case DXGI_FORMAT_R32G32_FLOAT:           return AF::Float32x2;
    case DXGI_FORMAT_R32G32B32_FLOAT:        return AF::Float32x3;
    case DXGI_FORMAT_R32G32B32A32_FLOAT:     return AF::Float32x4;
    case DXGI_FORMAT_R16G16_FLOAT:           return AF::Float16x2;
    case DXGI_FORMAT_R16G16B16A16_FLOAT:     return AF::Float16x4;
    case DXGI_FORMAT_R32_UINT:
    case DXGI_FORMAT_R32_SINT:               return AF::UInt32x1;
    case DXGI_FORMAT_R32G32_UINT:
    case DXGI_FORMAT_R32G32_SINT:            return AF::UInt32x2;
    case DXGI_FORMAT_R32G32B32_UINT:
    case DXGI_FORMAT_R32G32B32_SINT:         return AF::UInt32x3;
    case DXGI_FORMAT_R32G32B32A32_UINT:
    case DXGI_FORMAT_R32G32B32A32_SINT:      return AF::UInt32x4;
    case DXGI_FORMAT_R16G16_UINT:
    case DXGI_FORMAT_R16G16_SINT:            return AF::UInt16x2;
    case DXGI_FORMAT_R16G16B16A16_UINT:
    case DXGI_FORMAT_R16G16B16A16_SINT:      return AF::UInt16x4;
    case DXGI_FORMAT_R8G8B8A8_UINT:          return AF::UInt8x4;
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:   return AF::UNorm8x4;
    case DXGI_FORMAT_R8G8B8A8_SNORM:
    case DXGI_FORMAT_R16G16_SNORM:           return AF::SNorm16x2;
    case DXGI_FORMAT_R16G16B16A16_SNORM:     return AF::SNorm16x4;
    default:                                  return AF::Unknown;
    }
}

openripper::VertexSemantic semantic_name_to_enum(const char* name) noexcept {
    std::string n(name);
    for (char& c : n) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    if (n == "POSITION")                            return openripper::VertexSemantic::Position;
    if (n == "NORMAL")                              return openripper::VertexSemantic::Normal;
    if (n == "TANGENT")                             return openripper::VertexSemantic::Tangent;
    if (n == "BINORMAL" || n == "BITANGENT")        return openripper::VertexSemantic::Binormal;
    if (n == "TEXCOORD")                            return openripper::VertexSemantic::TexCoord;
    if (n == "COLOR")                               return openripper::VertexSemantic::Color;
    if (n == "BLENDINDICES")                        return openripper::VertexSemantic::BlendIndices;
    if (n == "BLENDWEIGHT" || n == "BLENDWEIGHTS")  return openripper::VertexSemantic::BlendWeights;
    return openripper::VertexSemantic::Custom;
}

constexpr std::uint32_t attr_format_bytes(AF f) noexcept {
    switch (f) {
    case AF::Float32x1:  return 4;  case AF::Float32x2:  return 8;
    case AF::Float32x3:  return 12; case AF::Float32x4:  return 16;
    case AF::Float16x2:  return 4;  case AF::Float16x4:  return 8;
    case AF::UInt32x1:   return 4;  case AF::UInt32x2:   return 8;
    case AF::UInt32x3:   return 12; case AF::UInt32x4:   return 16;
    case AF::UInt16x2:   return 4;  case AF::UInt16x4:   return 8;
    case AF::UInt8x4:    return 4;  case AF::SNorm16x2:  return 4;
    case AF::SNorm16x4:  return 8;  case AF::UNorm8x4:   return 4;
    default:             return 0;
    }
}

// ---- Fence-wait helper ----------------------------------------------------

bool fence_signal_wait(ID3D12CommandQueue* queue) {
    ++g_rb_fence_val;
    if (FAILED(queue->Signal(g_rb_fence, g_rb_fence_val))) {
        OR_LOG_WARN("d3d12 capture: Signal failed");
        return false;
    }
    if (g_rb_fence->GetCompletedValue() < g_rb_fence_val) {
        g_rb_fence->SetEventOnCompletion(g_rb_fence_val, g_rb_event);
        ::WaitForSingleObject(g_rb_event, 5000);
    }
    return true;
}

// ---- Readback a buffer region to a std::vector<std::byte> ----------------

bool readback_buffer_region(ID3D12Device* device, ID3D12CommandQueue* queue,
                             ID3D12Resource* src, UINT64 src_offset, UINT64 size,
                             std::vector<std::byte>& dst,
                             D3D12_RESOURCE_STATES assumed_src_state)
{
    // Create a READBACK heap buffer.
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension         = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width             = size;
    rd.Height            = rd.DepthOrArraySize = rd.MipLevels = 1;
    rd.Format            = DXGI_FORMAT_UNKNOWN;
    rd.Layout            = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    rd.SampleDesc.Count  = 1;

    ID3D12Resource* readback = nullptr;
    if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback)))) {
        OR_LOG_WARN("d3d12 capture: CreateCommittedResource(READBACK, {} bytes) failed", size);
        return false;
    }

    // Reset command list and record barrier + copy + barrier.
    g_rb_alloc->Reset();
    g_rb_cl->Reset(g_rb_alloc, nullptr);

    D3D12_RESOURCE_BARRIER b{};
    b.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource   = src;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = assumed_src_state;
    b.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE;
    g_rb_cl->ResourceBarrier(1, &b);

    g_rb_cl->CopyBufferRegion(readback, 0, src, src_offset, size);

    // Transition back to COMMON (safe landing state; game can promote from COMMON).
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    b.Transition.StateAfter  = D3D12_RESOURCE_STATE_COMMON;
    g_rb_cl->ResourceBarrier(1, &b);

    g_rb_cl->Close();
    ID3D12CommandList* lists[] = {g_rb_cl};
    queue->ExecuteCommandLists(1, lists);

    if (!fence_signal_wait(queue)) {
        readback->Release();
        return false;
    }

    // Map and copy.
    void* ptr = nullptr;
    D3D12_RANGE range{0, static_cast<SIZE_T>(size)};
    if (FAILED(readback->Map(0, &range, &ptr))) {
        OR_LOG_WARN("d3d12 capture: Map(READBACK) failed");
        readback->Release();
        return false;
    }
    dst.resize(static_cast<std::size_t>(size));
    std::memcpy(dst.data(), ptr, static_cast<std::size_t>(size));
    D3D12_RANGE null_range{0, 0};
    readback->Unmap(0, &null_range);
    readback->Release();
    return true;
}

// ---- Readback a Texture2D resource ----------------------------------------

bool readback_texture2d(ID3D12Device* device, ID3D12CommandQueue* queue,
                         ID3D12Resource* tex,
                         openripper::TextureSnapshot& snap_out,
                         D3D12_RESOURCE_STATES assumed_state)
{
    D3D12_RESOURCE_DESC desc = tex->GetDesc();
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D) return false;

    const UINT mips = desc.MipLevels;
    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints(mips);
    std::vector<UINT>   row_counts(mips);
    std::vector<UINT64> row_sizes(mips);
    UINT64 total_size = 0;
    device->GetCopyableFootprints(&desc, 0, mips, 0,
        footprints.data(), row_counts.data(), row_sizes.data(), &total_size);

    // Create READBACK buffer.
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension         = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width             = total_size;
    rd.Height            = rd.DepthOrArraySize = rd.MipLevels = 1;
    rd.Format            = DXGI_FORMAT_UNKNOWN;
    rd.Layout            = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    rd.SampleDesc.Count  = 1;

    ID3D12Resource* readback = nullptr;
    if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback)))) {
        OR_LOG_WARN("d3d12 capture: CreateCommittedResource(texture READBACK) failed");
        return false;
    }

    g_rb_alloc->Reset();
    g_rb_cl->Reset(g_rb_alloc, nullptr);

    // Barrier: assume PIXEL_SHADER_RESOURCE (most common texture state).
    D3D12_RESOURCE_BARRIER b{};
    b.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource   = tex;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = assumed_state;
    b.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE;
    g_rb_cl->ResourceBarrier(1, &b);

    for (UINT m = 0; m < mips; ++m) {
        D3D12_TEXTURE_COPY_LOCATION src_loc{};
        src_loc.pResource        = tex;
        src_loc.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src_loc.SubresourceIndex = m;

        D3D12_TEXTURE_COPY_LOCATION dst_loc{};
        dst_loc.pResource       = readback;
        dst_loc.Type            = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst_loc.PlacedFootprint = footprints[m];

        g_rb_cl->CopyTextureRegion(&dst_loc, 0, 0, 0, &src_loc, nullptr);
    }

    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    b.Transition.StateAfter  = D3D12_RESOURCE_STATE_COMMON;
    g_rb_cl->ResourceBarrier(1, &b);
    g_rb_cl->Close();

    ID3D12CommandList* lists[] = {g_rb_cl};
    queue->ExecuteCommandLists(1, lists);
    if (!fence_signal_wait(queue)) { readback->Release(); return false; }

    void* base_ptr = nullptr;
    D3D12_RANGE range{0, static_cast<SIZE_T>(total_size)};
    if (FAILED(readback->Map(0, &range, &base_ptr))) {
        readback->Release();
        return false;
    }

    snap_out.width       = static_cast<std::uint32_t>(desc.Width);
    snap_out.height      = desc.Height;
    snap_out.depth       = 1;
    snap_out.mip_levels  = mips;
    snap_out.array_size  = 1;
    snap_out.native_format = static_cast<std::uint32_t>(desc.Format);
    snap_out.subresources.resize(mips);

    for (UINT m = 0; m < mips; ++m) {
        const auto& fp  = footprints[m].Footprint;
        auto& sub       = snap_out.subresources[m];
        sub.mip_level   = m;
        sub.width       = fp.Width;
        sub.height      = fp.Height;
        sub.row_pitch   = fp.RowPitch;  // API row pitch; consumer will strip padding

        const std::size_t tight_row = (fp.Format != DXGI_FORMAT_UNKNOWN)
            ? static_cast<std::size_t>(row_sizes[m])
            : fp.RowPitch;

        sub.pixels.resize(static_cast<std::size_t>(fp.RowPitch) * fp.Height);
        const auto* src_row = static_cast<const std::byte*>(base_ptr)
                              + footprints[m].Offset;
        auto* dst_row = sub.pixels.data();
        for (UINT r = 0; r < fp.Height; ++r) {
            std::memcpy(dst_row, src_row, tight_row);
            src_row += fp.RowPitch;
            dst_row += fp.RowPitch;
        }
        // Tightly-packed row pitch for downstream consumers.
        sub.row_pitch = static_cast<std::uint32_t>(tight_row);
    }

    D3D12_RANGE null_range{0, 0};
    readback->Unmap(0, &null_range);
    readback->Release();
    return true;
}

} // namespace

// ---- Public API -----------------------------------------------------------

bool init_readback_infra(ID3D12Device* device) {
    if (g_rb_fence) return true; // already initialized

    if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                IID_PPV_ARGS(&g_rb_alloc)))) {
        OR_LOG_ERROR("d3d12 capture: CreateCommandAllocator failed");
        return false;
    }
    if (FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                g_rb_alloc, nullptr, IID_PPV_ARGS(&g_rb_cl)))) {
        OR_LOG_ERROR("d3d12 capture: CreateCommandList failed");
        g_rb_alloc->Release(); g_rb_alloc = nullptr;
        return false;
    }
    // Close immediately; will be Reset before each use.
    g_rb_cl->Close();

    if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_rb_fence)))) {
        OR_LOG_ERROR("d3d12 capture: CreateFence failed");
        g_rb_cl->Release(); g_rb_cl = nullptr;
        g_rb_alloc->Release(); g_rb_alloc = nullptr;
        return false;
    }
    g_rb_event = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
    OR_LOG_INFO("d3d12 capture: readback infrastructure ready");
    return true;
}

void shutdown_readback_infra() {
    if (g_rb_fence) { g_rb_fence->Release(); g_rb_fence = nullptr; }
    if (g_rb_cl)    { g_rb_cl->Release();    g_rb_cl    = nullptr; }
    if (g_rb_alloc) { g_rb_alloc->Release(); g_rb_alloc = nullptr; }
    if (g_rb_event) { ::CloseHandle(g_rb_event); g_rb_event = nullptr; }
    g_rb_fence_val = 0;
}

void capture_readback_defaults(std::vector<DrawRecord>& recs,
                                ID3D12Device* device,
                                ID3D12CommandQueue* queue,
                                std::vector<TextureReadback>& tex_out)
{
    if (!g_rb_fence) return;

    // Drain the game's queue once before issuing any copies.
    fence_signal_wait(queue);

    for (auto& rec : recs) {
        // ---- VB / IB DEFAULT-heap readbacks --------------------------------
        for (UINT i = 0; i < rec.vb_count; ++i) {
            if (!rec.vb_resources[i]) continue; // UPLOAD heap, already copied
            readback_buffer_region(device, queue,
                rec.vb_resources[i], rec.vb_byte_offsets[i], rec.vb_byte_sizes[i],
                rec.immediate_vb[i],
                D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
            rec.vb_resources[i]->Release();
            rec.vb_resources[i] = nullptr;
        }
        if (rec.ib_resource) {
            readback_buffer_region(device, queue,
                rec.ib_resource, rec.ib_byte_offset, rec.ib_byte_size,
                rec.immediate_ib,
                D3D12_RESOURCE_STATE_INDEX_BUFFER);
            rec.ib_resource->Release();
            rec.ib_resource = nullptr;
        }

        // ---- Texture readbacks ---------------------------------------------
        for (auto& [slot, res] : rec.textures) {
            TextureReadback tr;
            tr.draw_idx = rec.draw_id;
            tr.srv_slot = slot;
            if (readback_texture2d(device, queue, res, tr.snap,
                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)) {
                tr.snap.name = std::format("frame{:06}_draw{:05}_ps_t{}",
                                           rec.frame_id, rec.draw_id, slot);
                tex_out.push_back(std::move(tr));
            }
            res->Release();
            res = nullptr;
        }
        rec.textures.clear();
    }
}

std::optional<openripper::MeshSnapshot>
make_mesh_snapshot(const DrawRecord& rec, std::uint32_t draw_id, std::uint32_t frame_id)
{
    auto pso_layout = pso_lookup(rec.pso);
    if (!pso_layout || pso_layout->elements.empty()) {
        OR_LOG_WARN("d3d12 capture: no vertex layout for PSO {:p} (draw {})",
                    static_cast<void*>(rec.pso), draw_id);
        return std::nullopt;
    }

    // Decode vertex layout.
    openripper::VertexLayout layout;
    std::uint16_t running_offsets[k_max_vb_slots]{};

    bool has_position = false;
    for (const auto& el : pso_layout->elements) {
        openripper::VertexAttribute attr;
        attr.semantic       = semantic_name_to_enum(el.SemanticName);
        attr.semantic_index = static_cast<std::uint8_t>(el.SemanticIndex);
        attr.format         = dxgi_to_attr_format(el.Format);
        attr.stream         = static_cast<std::uint8_t>(el.InputSlot);

        if (el.AlignedByteOffset == D3D12_APPEND_ALIGNED_ELEMENT)
            attr.offset = running_offsets[attr.stream];
        else
            attr.offset = static_cast<std::uint16_t>(el.AlignedByteOffset);

        running_offsets[attr.stream] = attr.offset
            + static_cast<std::uint16_t>(attr_format_bytes(attr.format));

        if (attr.semantic == openripper::VertexSemantic::Position) has_position = true;
        layout.attributes.push_back(attr);
    }

    // Apply strides from VB views.
    for (UINT i = 0; i < rec.vb_count; ++i)
        layout.stream_strides[i] = static_cast<std::uint16_t>(rec.vb_views[i].StrideInBytes);

    // Heuristic: if no explicit Position semantic, promote first float3/float4 at slot0 offset0.
    if (!has_position && !layout.attributes.empty()) {
        for (auto& a : layout.attributes) {
            if (a.stream == 0 && a.offset == 0 &&
                (a.format == openripper::AttributeFormat::Float32x3 ||
                 a.format == openripper::AttributeFormat::Float32x4)) {
                a.semantic = openripper::VertexSemantic::Position;
                has_position = true;
                OR_LOG_DEBUG("d3d12 capture: promoted attribute at offset 0 to Position");
                break;
            }
        }
    }

    if (!has_position) {
        OR_LOG_WARN("d3d12 capture: no Position semantic in vertex layout (draw {})", draw_id);
        return std::nullopt;
    }

    // Build MeshSnapshot.
    openripper::MeshSnapshot snap;
    snap.name         = std::format("frame{:06}_draw{:05}", frame_id, draw_id);
    snap.layout       = std::move(layout);
    snap.draw_id      = draw_id;
    snap.frame_id     = frame_id;
    snap.start_index  = rec.start_index;
    snap.base_vertex  = rec.base_vertex;

    // Fill vertex streams.
    snap.vertex_streams.resize(rec.vb_count);
    for (UINT i = 0; i < rec.vb_count; ++i)
        snap.vertex_streams[i] = rec.immediate_vb[i];

    // Fill index buffer and topology.
    snap.index_buffer = rec.immediate_ib;
    if (rec.immediate_ib.empty()) {
        snap.index_format = openripper::IndexFormat::None;
        snap.vertex_count = rec.vertex_count;
    } else {
        const UINT ib_stride = (rec.ib_view.Format == DXGI_FORMAT_R16_UINT) ? 2 : 4;
        snap.index_count  = rec.index_count;
        snap.vertex_count = static_cast<std::uint32_t>(
            rec.immediate_ib.size() / ib_stride);
        snap.index_format = (ib_stride == 2)
            ? openripper::IndexFormat::U16
            : openripper::IndexFormat::U32;
    }

    switch (rec.topology) {
    case D3D_PRIMITIVE_TOPOLOGY_POINTLIST:     snap.topology = openripper::PrimitiveTopology::PointList;    break;
    case D3D_PRIMITIVE_TOPOLOGY_LINELIST:      snap.topology = openripper::PrimitiveTopology::LineList;     break;
    case D3D_PRIMITIVE_TOPOLOGY_LINESTRIP:     snap.topology = openripper::PrimitiveTopology::LineStrip;    break;
    case D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST:  snap.topology = openripper::PrimitiveTopology::TriangleList; break;
    case D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP: snap.topology = openripper::PrimitiveTopology::TriangleStrip;break;
    default:                                   snap.topology = openripper::PrimitiveTopology::Unknown;      break;
    }

    return snap;
}

} // namespace openripper::backends::d3d12
