// OpenRipper - src/backends/d3d11/capture_d3d11.cpp
//
// Full-frame GPU->CPU vertex/index capture for D3D11.
//
// Pipeline summary:
//   1. Bail out if context is deferred (staging read is illegal there).
//   2. Call IAGetPrimitiveTopology / IAGetInputLayout / IAGetIndexBuffer /
//      IAGetVertexBuffers to snapshot the IA state.
//   3. Look up the input layout in the registry built by the CreateInputLayout
//      hook to get the full D3D11_INPUT_ELEMENT_DESC[].
//   4. Decode each element into a openripper::VertexAttribute (semantic, format,
//      stream, byte-offset). Resolve D3D11_APPEND_ALIGNED_ELEMENT.
//   5. For each non-null vertex buffer and for the index buffer, create a
//      staging buffer, CopyResource, Map/memcpy/Unmap.
//   6. Return a MeshSnapshot with vertex_streams and index_buffer filled.

#include "capture_d3d11.hpp"
#include "state_d3d11.hpp"
#include "../../core/logger.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <format>
#include <string>
#include <vector>

namespace openripper::backends::d3d11 {
namespace {

// ---- Minimal RAII for COM pointers ----------------------------------------
// Avoids pulling in <wrl/client.h> while still being exception-safe.
template <typename T>
struct ComOwner {
    T* p = nullptr;
    ComOwner() = default;
    explicit ComOwner(T* ptr) noexcept : p(ptr) {}
    ~ComOwner() { if (p) p->Release(); }
    ComOwner(const ComOwner&)            = delete;
    ComOwner& operator=(const ComOwner&) = delete;
    T*  get()  const noexcept { return p; }
    T** put()        noexcept { return &p; }
    explicit operator bool() const noexcept { return p != nullptr; }
};

// ---- Byte-size of each AttributeFormat ------------------------------------
// Used to resolve D3D11_APPEND_ALIGNED_ELEMENT offsets and to size readbacks.
constexpr std::uint32_t attr_format_bytes(openripper::AttributeFormat f) noexcept {
    using AF = openripper::AttributeFormat;
    switch (f) {
    case AF::Float32x1:  return 4;
    case AF::Float32x2:  return 8;
    case AF::Float32x3:  return 12;
    case AF::Float32x4:  return 16;
    case AF::Float16x2:  return 4;
    case AF::Float16x4:  return 8;
    case AF::UInt32x1:   return 4;
    case AF::UInt32x2:   return 8;
    case AF::UInt32x3:   return 12;
    case AF::UInt32x4:   return 16;
    case AF::UInt16x2:   return 4;
    case AF::UInt16x4:   return 8;
    case AF::UInt8x4:    return 4;
    case AF::SNorm16x2:  return 4;
    case AF::SNorm16x4:  return 8;
    case AF::UNorm8x4:   return 4;
    default:             return 0;
    }
}

// ---- DXGI_FORMAT -> AttributeFormat ---------------------------------------
openripper::AttributeFormat dxgi_to_attr_format(DXGI_FORMAT f) noexcept {
    using AF = openripper::AttributeFormat;
    switch (f) {
    case DXGI_FORMAT_R32_FLOAT:              return AF::Float32x1;
    case DXGI_FORMAT_R32G32_FLOAT:           return AF::Float32x2;
    case DXGI_FORMAT_R32G32B32_FLOAT:        return AF::Float32x3;
    case DXGI_FORMAT_R32G32B32A32_FLOAT:     return AF::Float32x4;
    case DXGI_FORMAT_R16G16_FLOAT:           return AF::Float16x2;
    case DXGI_FORMAT_R16G16B16A16_FLOAT:     return AF::Float16x4;
    // Signed int variants: store in the unsigned slot; OBJ export ignores them.
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
    case DXGI_FORMAT_R8G8B8A8_SNORM:        // rare but valid in vertex buffers
    case DXGI_FORMAT_R16G16_SNORM:           return AF::SNorm16x2;
    case DXGI_FORMAT_R16G16B16A16_SNORM:     return AF::SNorm16x4;
    default:                                  return AF::Unknown;
    }
}

// ---- Semantic string -> VertexSemantic ------------------------------------
openripper::VertexSemantic semantic_name_to_enum(const char* name) noexcept {
    // Uppercase comparison; HLSL semantic names are case-insensitive.
    std::string n(name);
    for (char& c : n) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));

    if (n == "POSITION")                             return openripper::VertexSemantic::Position;
    if (n == "NORMAL")                               return openripper::VertexSemantic::Normal;
    if (n == "TANGENT")                              return openripper::VertexSemantic::Tangent;
    if (n == "BINORMAL"  || n == "BITANGENT")        return openripper::VertexSemantic::Binormal;
    if (n == "TEXCOORD")                             return openripper::VertexSemantic::TexCoord;
    if (n == "COLOR")                                return openripper::VertexSemantic::Color;
    if (n == "BLENDINDICES")                         return openripper::VertexSemantic::BlendIndices;
    if (n == "BLENDWEIGHT" || n == "BLENDWEIGHTS")   return openripper::VertexSemantic::BlendWeights;
    if (n == "PSIZE"      || n == "POINTSIZE")       return openripper::VertexSemantic::PointSize;
    if (n == "FOG")                                  return openripper::VertexSemantic::Fog;
    return openripper::VertexSemantic::Custom;
}

// ---- D3D11_PRIMITIVE_TOPOLOGY -> PrimitiveTopology ------------------------
openripper::PrimitiveTopology d3d11_topology_to_core(D3D11_PRIMITIVE_TOPOLOGY t) noexcept {
    switch (t) {
    case D3D11_PRIMITIVE_TOPOLOGY_POINTLIST:     return openripper::PrimitiveTopology::PointList;
    case D3D11_PRIMITIVE_TOPOLOGY_LINELIST:      return openripper::PrimitiveTopology::LineList;
    case D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP:     return openripper::PrimitiveTopology::LineStrip;
    case D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST:  return openripper::PrimitiveTopology::TriangleList;
    case D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP: return openripper::PrimitiveTopology::TriangleStrip;
    default:                                      return openripper::PrimitiveTopology::Unknown;
    }
}

// ---- GPU -> CPU buffer copy -----------------------------------------------
// Creates a D3D11_USAGE_STAGING mirror of src, copies it, and memcpy's the
// result into dst. The staging buffer is released on return.
bool readback_buffer(ID3D11Device*          device,
                     ID3D11DeviceContext*    ctx,
                     ID3D11Buffer*           src,
                     std::vector<std::byte>& dst)
{
    D3D11_BUFFER_DESC src_desc{};
    src->GetDesc(&src_desc);

    // Staging buffers must have BindFlags = 0 and MiscFlags = 0.
    D3D11_BUFFER_DESC stg{};
    stg.ByteWidth      = src_desc.ByteWidth;
    stg.Usage          = D3D11_USAGE_STAGING;
    stg.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

    ComOwner<ID3D11Buffer> staging;
    if (FAILED(device->CreateBuffer(&stg, nullptr, staging.put()))) {
        OR_LOG_WARN("capture: CreateBuffer(STAGING, {} bytes) failed", src_desc.ByteWidth);
        return false;
    }

    ctx->CopyResource(staging.get(), src);

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(ctx->Map(staging.get(), 0, D3D11_MAP_READ, 0, &mapped))) {
        OR_LOG_WARN("capture: Map(STAGING) failed");
        return false;
    }

    dst.resize(src_desc.ByteWidth);
    std::memcpy(dst.data(), mapped.pData, src_desc.ByteWidth);
    ctx->Unmap(staging.get(), 0);
    return true;
}

} // namespace (anonymous)

// ---- Public entry point ---------------------------------------------------

std::optional<openripper::MeshSnapshot> capture_draw(ID3D11DeviceContext* ctx,
                                                std::uint32_t        index_count,
                                                std::uint32_t        vertex_count,
                                                std::uint32_t        /*start_index*/,
                                                std::int32_t         /*base_vertex*/,
                                                std::uint32_t        draw_id,
                                                std::uint32_t        frame_id)
{
    // ---- Guard: deferred contexts cannot be staged from CPU ---------------
    // Deferred-context capture requires command-list replay; deferred to a
    // later stage.
    if (ctx->GetType() == D3D11_DEVICE_CONTEXT_DEFERRED) {
        static bool warned = false;
        if (!warned) {
            OR_LOG_WARN("capture: deferred context detected - skipping (not supported in stage 2)");
            warned = true;
        }
        return std::nullopt;
    }

    // ---- Acquire device (ADDREF via GetDevice, released by ComOwner) ------
    ComOwner<ID3D11Device> device;
    ctx->GetDevice(device.put());
    if (!device) {
        OR_LOG_WARN("capture: GetDevice returned null");
        return std::nullopt;
    }

    // ---- IA state snapshot ------------------------------------------------
    D3D11_PRIMITIVE_TOPOLOGY d3d_topo{};
    ctx->IAGetPrimitiveTopology(&d3d_topo);

    ComOwner<ID3D11InputLayout> il;
    ctx->IAGetInputLayout(il.put());
    if (!il) {
        // A draw call with no input layout is legal in D3D11 (e.g. the vertex
        // shader uses SV_VertexID only), but we have nothing to capture.
        OR_LOG_TRACE("capture: draw {} has no input layout - skipping", draw_id);
        return std::nullopt;
    }

    auto il_desc_opt = lookup_input_layout(il.get());
    if (!il_desc_opt) {
        // This fires when a game creates layouts before our hook was installed.
        // Log once per unique pointer so we don't spam.
        OR_LOG_WARN("capture: input layout {:p} not in registry (created pre-hook?)",
                    static_cast<void*>(il.get()));
        return std::nullopt;
    }
    const InputLayoutDesc& il_desc    = *il_desc_opt;
    const std::size_t      elem_count = il_desc.elements.size();

    // Determine the highest vertex-buffer slot the layout references so we
    // only call IAGetVertexBuffers for the slots we actually need.
    UINT max_slot = 0;
    for (const auto& e : il_desc.elements)
        max_slot = std::max(max_slot, e.InputSlot);

    constexpr UINT kMaxSlots  = D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT; // 32
    const UINT     slot_count = std::min(max_slot + 1, kMaxSlots);

    // ---- Vertex buffers (IAGetVertexBuffers ADDREFs each non-null entry) --
    // Keep everything in a RAII block so we can early-return freely.
    struct VBGuard {
        ID3D11Buffer* ptrs[kMaxSlots]{};
        UINT strides[kMaxSlots]{};
        UINT offsets[kMaxSlots]{};
        UINT bound = 0;
        ~VBGuard() {
            for (UINT i = 0; i < bound; ++i)
                if (ptrs[i]) ptrs[i]->Release();
        }
    } vb;
    vb.bound = slot_count;
    ctx->IAGetVertexBuffers(0, slot_count, vb.ptrs, vb.strides, vb.offsets);

    // ---- Index buffer -----------------------------------------------------
    ComOwner<ID3D11Buffer> ib;
    DXGI_FORMAT            ib_fmt    = DXGI_FORMAT_UNKNOWN;
    UINT                   ib_offset = 0;
    // IAGetIndexBuffer ADDREFs *ppIndexBuffer if non-null.
    ctx->IAGetIndexBuffer(ib.put(), &ib_fmt, &ib_offset);

    // ---- Build VertexLayout -----------------------------------------------
    openripper::MeshSnapshot mesh;
    mesh.draw_id  = draw_id;
    mesh.frame_id = frame_id;
    mesh.name     = std::format("frame{:06}_draw{:05}", frame_id, draw_id);
    mesh.topology = d3d11_topology_to_core(d3d_topo);

    // Populate per-slot strides (from what the app bound, not the layout desc).
    for (UINT s = 0; s < slot_count && s < 16; ++s)
        mesh.layout.stream_strides[s] = static_cast<std::uint16_t>(vb.strides[s]);

    // Build attribute list; resolve D3D11_APPEND_ALIGNED_ELEMENT offsets.
    mesh.layout.attributes.reserve(elem_count);
    {
        // packed[s] tracks the next available byte offset in stream s when
        // APPEND_ALIGNED_ELEMENT is used.
        UINT packed[kMaxSlots]{};

        for (std::size_t i = 0; i < elem_count; ++i) {
            const D3D11_INPUT_ELEMENT_DESC& e = il_desc.elements[i];
            openripper::VertexAttribute attr{};
            attr.semantic       = semantic_name_to_enum(e.SemanticName);
            attr.semantic_index = static_cast<std::uint8_t>(e.SemanticIndex);
            attr.format         = dxgi_to_attr_format(e.Format);
            attr.stream         = static_cast<std::uint8_t>(e.InputSlot);

            if (e.AlignedByteOffset == D3D11_APPEND_ALIGNED_ELEMENT) {
                // Pack immediately after the previous element in this stream.
                attr.offset = static_cast<std::uint16_t>(packed[e.InputSlot]);
            } else {
                attr.offset = static_cast<std::uint16_t>(e.AlignedByteOffset);
            }

            // Advance the running offset for this stream.
            packed[e.InputSlot] = attr.offset + attr_format_bytes(attr.format);

            mesh.layout.attributes.push_back(attr);
        }
    }

    // ---- GPU -> CPU: vertex buffers ---------------------------------------
    mesh.vertex_streams.resize(slot_count);
    for (UINT s = 0; s < slot_count; ++s) {
        if (!vb.ptrs[s]) continue;   // slot not bound
        if (!readback_buffer(device.get(), ctx, vb.ptrs[s], mesh.vertex_streams[s]))
            OR_LOG_WARN("capture: VB slot {} readback failed in draw {}", s, draw_id);
    }

    // ---- GPU -> CPU: index buffer -----------------------------------------
    const bool has_index = (ib && index_count > 0 &&
                             (ib_fmt == DXGI_FORMAT_R16_UINT ||
                              ib_fmt == DXGI_FORMAT_R32_UINT));
    if (has_index) {
        mesh.index_format = (ib_fmt == DXGI_FORMAT_R32_UINT)
                            ? openripper::IndexFormat::U32
                            : openripper::IndexFormat::U16;
        mesh.index_count  = index_count;
        if (!readback_buffer(device.get(), ctx, ib.get(), mesh.index_buffer)) {
            OR_LOG_WARN("capture: IB readback failed in draw {} - exporting non-indexed", draw_id);
            mesh.index_format = openripper::IndexFormat::None;
            mesh.index_count  = 0;
        }
    } else {
        mesh.index_format = openripper::IndexFormat::None;
        mesh.index_count  = 0;
    }

    mesh.vertex_count = vertex_count;  // draw-call argument; whole VB was captured

    OR_LOG_DEBUG("capture: draw {} - {} attrs, vcount={}, icount={}",
                 draw_id, elem_count, vertex_count, mesh.index_count);
    return mesh;
}

} // namespace openripper::backends::d3d11
