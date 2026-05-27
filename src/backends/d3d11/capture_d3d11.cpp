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
#include "runtime_state.hpp"
#include "state_d3d11.hpp"
#include "../../core/logger.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <format>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace openripper::backends::d3d11 {

// ---- Per-frame texture dedup map -------------------------------------------
// Keyed on ID3D11Texture2D* (pointer identity = same GPU resource).
// Cleared at the start of each capture frame via dedup_begin_frame().
namespace {
struct DedupEntry {
    std::string   file;
    std::uint32_t native_format{0};
    std::uint32_t width{0};
    std::uint32_t height{0};
    std::uint32_t mip_levels{0};
};
std::unordered_map<ID3D11Texture2D*, DedupEntry> s_tex_dedup;
} // anonymous ns

void dedup_begin_frame() { s_tex_dedup.clear(); }

void dedup_tex_register(ID3D11Texture2D*                   ptr,
                        std::string_view                   filename,
                        const openripper::TextureSnapshot& snap_meta)
{
    s_tex_dedup.insert_or_assign(ptr, DedupEntry{
        std::string(filename),
        snap_meta.native_format,
        snap_meta.width,
        snap_meta.height,
        snap_meta.mip_levels
    });
}

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

// ---- Short display name for AttributeFormat (debug logs) ------------------
constexpr const char* attr_format_name(openripper::AttributeFormat f) noexcept {
    using AF = openripper::AttributeFormat;
    switch (f) {
    case AF::Float32x1: return "F32x1";
    case AF::Float32x2: return "F32x2";
    case AF::Float32x3: return "F32x3";
    case AF::Float32x4: return "F32x4";
    case AF::Float16x2: return "F16x2";
    case AF::Float16x4: return "F16x4";
    case AF::UInt32x1:  return "U32x1";
    case AF::UInt32x2:  return "U32x2";
    case AF::UInt32x3:  return "U32x3";
    case AF::UInt32x4:  return "U32x4";
    case AF::UInt16x2:  return "U16x2";
    case AF::UInt16x4:  return "U16x4";
    case AF::UInt8x4:   return "U8x4";
    case AF::SNorm16x2: return "SN16x2";
    case AF::SNorm16x4: return "SN16x4";
    case AF::UNorm8x4:  return "UN8x4";
    default:            return "?";
    }
}

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

// ---- Texture format helpers ------------------------------------------------

// BC ranges in DXGI_FORMAT: BC1-BC5 = 70-84, BC6H-BC7 = 94-99.
// 85-93 are non-BC B-channel formats (B5G6R5, B8G8R8A8, etc.) — not block-compressed.
bool is_block_compressed(DXGI_FORMAT fmt) noexcept {
    const auto v = static_cast<std::uint32_t>(fmt);
    return (v >= 70u && v <= 84u) || (v >= 94u && v <= 99u);
}

// Bytes per 4x4 texel block for BC formats. BC1 and BC4 = 8 bytes; all others = 16.
std::uint32_t bc_bytes_per_block(DXGI_FORMAT fmt) noexcept {
    switch (fmt) {
    case DXGI_FORMAT_BC1_TYPELESS:
    case DXGI_FORMAT_BC1_UNORM:
    case DXGI_FORMAT_BC1_UNORM_SRGB:
    case DXGI_FORMAT_BC4_TYPELESS:
    case DXGI_FORMAT_BC4_UNORM:
    case DXGI_FORMAT_BC4_SNORM:       return 8;
    default:                           return 16;
    }
}

// Bytes per pixel for common uncompressed DXGI formats. Returns 0 for unknown.
std::uint32_t bytes_per_pixel_uncompressed(DXGI_FORMAT fmt) noexcept {
    switch (fmt) {
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
    case DXGI_FORMAT_R32G32B32A32_UINT:
    case DXGI_FORMAT_R32G32B32A32_SINT:    return 16;
    case DXGI_FORMAT_R32G32B32_FLOAT:
    case DXGI_FORMAT_R32G32B32_UINT:
    case DXGI_FORMAT_R32G32B32_SINT:       return 12;
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_UNORM:
    case DXGI_FORMAT_R16G16B16A16_UINT:
    case DXGI_FORMAT_R16G16B16A16_SNORM:
    case DXGI_FORMAT_R16G16B16A16_SINT:
    case DXGI_FORMAT_R32G32_FLOAT:
    case DXGI_FORMAT_R32G32_UINT:
    case DXGI_FORMAT_R32G32_SINT:          return 8;
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_R8G8B8A8_UINT:
    case DXGI_FORMAT_R8G8B8A8_SNORM:
    case DXGI_FORMAT_R8G8B8A8_SINT:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8X8_UNORM:
    case DXGI_FORMAT_R10G10B10A2_UNORM:
    case DXGI_FORMAT_R10G10B10A2_UINT:
    case DXGI_FORMAT_R11G11B10_FLOAT:
    case DXGI_FORMAT_R32_FLOAT:
    case DXGI_FORMAT_R32_UINT:
    case DXGI_FORMAT_R32_SINT:
    case DXGI_FORMAT_D32_FLOAT:
    case DXGI_FORMAT_D24_UNORM_S8_UINT:    return 4;
    case DXGI_FORMAT_R16G16_FLOAT:
    case DXGI_FORMAT_R16G16_UNORM:
    case DXGI_FORMAT_R16G16_UINT:
    case DXGI_FORMAT_R16G16_SNORM:
    case DXGI_FORMAT_R16G16_SINT:
    case DXGI_FORMAT_R16_FLOAT:
    case DXGI_FORMAT_D16_UNORM:
    case DXGI_FORMAT_R16_UNORM:
    case DXGI_FORMAT_R16_UINT:
    case DXGI_FORMAT_R16_SNORM:
    case DXGI_FORMAT_R16_SINT:
    case DXGI_FORMAT_R8G8_UNORM:
    case DXGI_FORMAT_R8G8_UINT:
    case DXGI_FORMAT_R8G8_SNORM:
    case DXGI_FORMAT_R8G8_SINT:
    case DXGI_FORMAT_B5G6R5_UNORM:
    case DXGI_FORMAT_B5G5R5A1_UNORM:       return 2;
    case DXGI_FORMAT_R8_UNORM:
    case DXGI_FORMAT_R8_UINT:
    case DXGI_FORMAT_R8_SNORM:
    case DXGI_FORMAT_R8_SINT:
    case DXGI_FORMAT_A8_UNORM:             return 1;
    default:                                return 0;
    }
}

// Tightly-packed bytes per logical row (no API RowPitch padding).
std::uint32_t bytes_per_logical_row(DXGI_FORMAT fmt, UINT width) noexcept {
    if (is_block_compressed(fmt))
        return std::max(1u, (width + 3u) / 4u) * bc_bytes_per_block(fmt);
    return width * bytes_per_pixel_uncompressed(fmt);
}

// Number of logical rows (for BC, one row of blocks covers 4 pixel rows).
UINT logical_row_count(DXGI_FORMAT fmt, UINT height) noexcept {
    if (is_block_compressed(fmt))
        return std::max(1u, (height + 3u) / 4u);
    return height;
}

// ---- GPU -> CPU texture copy -----------------------------------------------
// Creates a staging mirror of a 2D texture, copies it, and reads back every
// mip of array slice 0. The staging texture is released on return.
// MSAA and unknown formats are skipped with a warning.
bool readback_texture2d(ID3D11Device*               device,
                        ID3D11DeviceContext*         ctx,
                        ID3D11Texture2D*             src,
                        openripper::TextureSnapshot& snap)
{
    D3D11_TEXTURE2D_DESC src_desc{};
    src->GetDesc(&src_desc);

    if (src_desc.SampleDesc.Count > 1) {
        OR_LOG_WARN("capture_tex: MSAA texture ({} samples) - skipping (resolve not implemented)",
                    src_desc.SampleDesc.Count);
        return false;
    }

    const DXGI_FORMAT fmt = static_cast<DXGI_FORMAT>(src_desc.Format);
    if (!is_block_compressed(fmt) && bytes_per_pixel_uncompressed(fmt) == 0) {
        OR_LOG_WARN("capture_tex: unsupported/typeless format {} - skipping",
                    static_cast<std::uint32_t>(fmt));
        return false;
    }

    D3D11_TEXTURE2D_DESC stg_desc = src_desc;
    stg_desc.Usage          = D3D11_USAGE_STAGING;
    stg_desc.BindFlags      = 0;
    stg_desc.MiscFlags      = 0;  // drop TEXTURECUBE / GENERATE_MIPS / shared flags
    stg_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

    ComOwner<ID3D11Texture2D> staging;
    if (FAILED(device->CreateTexture2D(&stg_desc, nullptr, staging.put()))) {
        OR_LOG_WARN("capture_tex: CreateTexture2D(STAGING) failed (fmt={})",
                    static_cast<std::uint32_t>(fmt));
        return false;
    }

    ctx->CopyResource(staging.get(), src);

    snap.subresources.reserve(src_desc.MipLevels);

    for (UINT m = 0; m < src_desc.MipLevels; ++m) {
        const UINT sub = D3D11CalcSubresource(m, 0, src_desc.MipLevels);

        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(ctx->Map(staging.get(), sub, D3D11_MAP_READ, 0, &mapped))) {
            OR_LOG_WARN("capture_tex: Map(mip {}) failed - aborting readback", m);
            ctx->Unmap(staging.get(), sub);
            snap.subresources.clear();
            return false;
        }

        const UINT mip_w     = std::max(1u, src_desc.Width  >> m);
        const UINT mip_h     = std::max(1u, src_desc.Height >> m);
        const UINT tight_row = bytes_per_logical_row(fmt, mip_w);
        const UINT row_count = logical_row_count(fmt, mip_h);

        openripper::TextureSubresource sub_out;
        sub_out.mip_level = m;
        sub_out.width     = mip_w;
        sub_out.height    = mip_h;
        sub_out.row_pitch = tight_row;
        sub_out.pixels.resize(static_cast<std::size_t>(tight_row) * row_count);

        const auto* src_row = static_cast<const std::byte*>(mapped.pData);
        auto*       dst_row = sub_out.pixels.data();
        for (UINT r = 0; r < row_count; ++r) {
            std::memcpy(dst_row, src_row, tight_row);
            src_row += mapped.RowPitch;
            dst_row += tight_row;
        }

        ctx->Unmap(staging.get(), sub);
        snap.subresources.push_back(std::move(sub_out));
    }

    snap.width         = src_desc.Width;
    snap.height        = src_desc.Height;
    snap.depth         = 1;  // ID3D11Texture2D is always depth=1
    snap.mip_levels    = src_desc.MipLevels;
    snap.array_size    = src_desc.ArraySize;
    snap.native_format = static_cast<std::uint32_t>(src_desc.Format);
    return true;
}

} // namespace (anonymous)

// ---- Public entry point ---------------------------------------------------

std::optional<openripper::MeshSnapshot> capture_draw(ID3D11DeviceContext* ctx,
                                                std::uint32_t        index_count,
                                                std::uint32_t        vertex_count,
                                                std::uint32_t        start_index,
                                                std::int32_t         base_vertex,
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
        bool position_found = false;

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

            // Track canonical Position hits.
            using VS = openripper::VertexSemantic;
            using AF = openripper::AttributeFormat;
            if (attr.semantic == VS::Position) {
                position_found = true;
            } else if (!position_found && attr.semantic == VS::Custom &&
                       attr.stream == 0 && attr.offset == 0 &&
                       (attr.format == AF::Float32x3 || attr.format == AF::Float32x4)) {
                // Heuristic: first float3/4 at slot 0, offset 0 with an unrecognised
                // semantic name is almost certainly the vertex position (e.g. SV_Position,
                // ATTR0, VPOS, or engine-specific names).
                attr.semantic = VS::Position;
                position_found = true;
                OR_LOG_DEBUG("capture: draw {} promoting '{}{}' to Position (heuristic)",
                             draw_id, e.SemanticName,
                             e.SemanticIndex ? std::to_string(e.SemanticIndex) : "");
            }

            mesh.layout.attributes.push_back(attr);
        }
    }

    // Debug: emit one line showing every semantic + format so users can diagnose
    // unrecognised engines without guessing.
    {
        std::string s;
        for (std::size_t i = 0; i < elem_count; ++i) {
            const auto& e = il_desc.elements[i];
            const auto& a = mesh.layout.attributes[i];
            if (i) s += ' ';
            s += e.SemanticName;
            if (e.SemanticIndex) s += std::to_string(e.SemanticIndex);
            s += '/';
            s += attr_format_name(a.format);
        }
        OR_LOG_DEBUG("capture: draw {} layout: {}", draw_id, s);
    }

    // ---- GPU -> CPU: vertex buffers ---------------------------------------
    mesh.vertex_streams.resize(slot_count);
    for (UINT s = 0; s < slot_count; ++s) {
        if (!vb.ptrs[s]) continue;   // slot not bound
        if (!readback_buffer(device.get(), ctx, vb.ptrs[s], mesh.vertex_streams[s])) {
            OR_LOG_WARN("capture: VB slot {} readback failed in draw {}", s, draw_id);
            continue;
        }
        // Slice by IASetVertexBuffers offset so vertex row 0 = first vertex of draw.
        if (vb.offsets[s] > 0 && vb.offsets[s] < mesh.vertex_streams[s].size()) {
            mesh.vertex_streams[s].erase(
                mesh.vertex_streams[s].begin(),
                mesh.vertex_streams[s].begin() + vb.offsets[s]);
        }
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
        } else if (ib_offset > 0 && ib_offset < mesh.index_buffer.size()) {
            mesh.index_buffer.erase(
                mesh.index_buffer.begin(),
                mesh.index_buffer.begin() + ib_offset);
        }
    } else {
        mesh.index_format = openripper::IndexFormat::None;
        mesh.index_count  = 0;
    }

    mesh.vertex_count = vertex_count;
    mesh.start_index  = start_index;
    mesh.base_vertex  = base_vertex;

    OR_LOG_DEBUG("capture: draw {} - {} attrs, vcount={}, icount={}",
                 draw_id, elem_count, vertex_count, mesh.index_count);
    return mesh;
}

// ---- Texture capture entry point ------------------------------------------

std::vector<CaptureTexResult>
capture_pixel_textures(ID3D11DeviceContext* ctx,
                       std::uint32_t        draw_id,
                       std::uint32_t        frame_id)
{
    std::vector<CaptureTexResult> results;

    if (ctx->GetType() == D3D11_DEVICE_CONTEXT_DEFERRED) {
        static bool warned = false;
        if (!warned) {
            OR_LOG_WARN("capture_tex: deferred context - skipping (immediate context only)");
            warned = true;
        }
        return results;
    }

    ComOwner<ID3D11Device> device;
    ctx->GetDevice(device.put());
    if (!device) return results;

    // PSGetShaderResources AddRefs every non-null pointer; ComOwner releases them.
    constexpr UINT k_slots = D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT; // 128
    ID3D11ShaderResourceView* raw_srvs[k_slots]{};
    ctx->PSGetShaderResources(0, k_slots, raw_srvs);

    for (UINT slot = 0; slot < k_slots; ++slot) {
        if (!raw_srvs[slot]) continue;
        ComOwner<ID3D11ShaderResourceView> srv(raw_srvs[slot]);

        D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc{};
        srv.get()->GetDesc(&srv_desc);

        if (srv_desc.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2D) {
            OR_LOG_TRACE("capture_tex: draw {} slot {} dim={} - not Texture2D, skipping",
                         draw_id, slot, static_cast<int>(srv_desc.ViewDimension));
            continue;
        }

        ComOwner<ID3D11Resource> res;
        srv.get()->GetResource(res.put());
        if (!res) continue;

        ComOwner<ID3D11Texture2D> tex2d;
        res.get()->QueryInterface(__uuidof(ID3D11Texture2D),
                                  reinterpret_cast<void**>(tex2d.put()));
        if (!tex2d) continue;

        // Dedup: if we've already captured this texture pointer this frame, skip readback.
        if (g_dedup) {
            auto it = s_tex_dedup.find(tex2d.get());
            if (it != s_tex_dedup.end()) {
                const auto& de = it->second;
                OR_LOG_DEBUG("dedup: draw {} slot {} -> reusing {}", draw_id, slot, de.file);
                CaptureTexResult r;
                r.slot       = slot;
                r.source_ptr = tex2d.get();
                r.reuse_file = de.file;
                r.reuse_fmt  = de.native_format;
                r.reuse_w    = de.width;
                r.reuse_h    = de.height;
                r.reuse_mips = de.mip_levels;
                results.push_back(std::move(r));
                continue;
            }
        }

        openripper::TextureSnapshot snap;
        snap.name = std::format("frame{:06}_draw{:05}_ps_t{}", frame_id, draw_id, slot);

        if (!readback_texture2d(device.get(), ctx, tex2d.get(), snap)) {
            OR_LOG_WARN("capture_tex: readback failed for draw {} slot {}", draw_id, slot);
            continue;
        }

        OR_LOG_DEBUG("capture_tex: draw {} slot {} -> {}x{} dxgi={} mips={}",
                     draw_id, slot,
                     snap.width, snap.height, snap.native_format, snap.mip_levels);
        CaptureTexResult r;
        r.slot       = slot;
        r.source_ptr = tex2d.get(); // non-owning key; game holds its own ref
        r.snap       = std::move(snap);
        results.push_back(std::move(r));
    }

    return results;
}

} // namespace openripper::backends::d3d11
