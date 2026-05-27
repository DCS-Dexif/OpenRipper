// OpenRipper - src/backends/d3d9/capture_d3d9.cpp
//
// Mesh and texture capture for D3D9.
//
// D3D9 is a synchronous API: vertex/index buffers can be Locked immediately
// on the CPU thread without GPU fences. We lock with D3DLOCK_READONLY and
// memcpy the relevant byte range.
//
// Vertex layout comes from IDirect3DVertexDeclaration9 (preferred) or the
// legacy FVF (fallback). State is read via Get* device methods rather than
// hooking Set* methods, keeping the hook count minimal.

#include "capture_d3d9.hpp"
#include "runtime_state.hpp"
#include "../../core/logger.hpp"

#include <dxgi.h>

#include <algorithm>
#include <cstring>
#include <format>
#include <set>
#include <string_view>
#include <unordered_map>

namespace openripper::backends::d3d9 {

// ---- Per-frame texture dedup map -------------------------------------------
namespace {
struct DedupEntry9 {
    std::string   file;
    std::uint32_t native_format{0};
    std::uint32_t width{0};
    std::uint32_t height{0};
    std::uint32_t mip_levels{0};
};
std::unordered_map<IDirect3DTexture9*, DedupEntry9> s_tex_dedup;
} // anonymous ns

void dedup_begin_frame() { s_tex_dedup.clear(); }

void dedup_tex_register(IDirect3DTexture9*                 ptr,
                        std::string_view                   filename,
                        const openripper::TextureSnapshot& snap_meta)
{
    s_tex_dedup.insert_or_assign(ptr, DedupEntry9{
        std::string(filename),
        snap_meta.native_format,
        snap_meta.width,
        snap_meta.height,
        snap_meta.mip_levels
    });
}

namespace {

// ---- Primitive helpers -------------------------------------------------------

UINT verts_from_prims(D3DPRIMITIVETYPE t, UINT n) noexcept {
    switch (t) {
    case D3DPT_POINTLIST:     return n;
    case D3DPT_LINELIST:      return n * 2;
    case D3DPT_LINESTRIP:     return n + 1;
    case D3DPT_TRIANGLELIST:  return n * 3;
    case D3DPT_TRIANGLESTRIP: return n + 2;
    case D3DPT_TRIANGLEFAN:   return n + 2;
    default:                  return n * 3;
    }
}

openripper::PrimitiveTopology d3dprim_to_topo(D3DPRIMITIVETYPE t) noexcept {
    switch (t) {
    case D3DPT_POINTLIST:     return openripper::PrimitiveTopology::PointList;
    case D3DPT_LINELIST:      return openripper::PrimitiveTopology::LineList;
    case D3DPT_LINESTRIP:     return openripper::PrimitiveTopology::LineStrip;
    case D3DPT_TRIANGLELIST:  return openripper::PrimitiveTopology::TriangleList;
    case D3DPT_TRIANGLESTRIP: return openripper::PrimitiveTopology::TriangleStrip;
    case D3DPT_TRIANGLEFAN:   return openripper::PrimitiveTopology::TriangleFan;
    default:                  return openripper::PrimitiveTopology::Unknown;
    }
}

// ---- Vertex layout decoders --------------------------------------------------

using AF = openripper::AttributeFormat;
using VS = openripper::VertexSemantic;

AF d3dtype_to_af(BYTE type) noexcept {
    switch (type) {
    case D3DDECLTYPE_FLOAT1:    return AF::Float32x1;
    case D3DDECLTYPE_FLOAT2:    return AF::Float32x2;
    case D3DDECLTYPE_FLOAT3:    return AF::Float32x3;
    case D3DDECLTYPE_FLOAT4:    return AF::Float32x4;
    case D3DDECLTYPE_D3DCOLOR:  return AF::UNorm8x4;   // BGRA packed
    case D3DDECLTYPE_UBYTE4:    return AF::UInt8x4;
    case D3DDECLTYPE_SHORT2:    return AF::SNorm16x2;
    case D3DDECLTYPE_SHORT4:    return AF::SNorm16x4;
    case D3DDECLTYPE_UBYTE4N:   return AF::UNorm8x4;
    case D3DDECLTYPE_SHORT2N:   return AF::SNorm16x2;
    case D3DDECLTYPE_SHORT4N:   return AF::SNorm16x4;
    case D3DDECLTYPE_USHORT2N:  return AF::UInt16x2;
    case D3DDECLTYPE_USHORT4N:  return AF::UInt16x4;
    case D3DDECLTYPE_UDEC3:     return AF::UInt32x1;
    case D3DDECLTYPE_DEC3N:     return AF::UInt32x1;
    case D3DDECLTYPE_FLOAT16_2: return AF::Float16x2;
    case D3DDECLTYPE_FLOAT16_4: return AF::Float16x4;
    default:                    return AF::Unknown;
    }
}

VS d3dusage_to_sem(BYTE usage) noexcept {
    switch (usage) {
    case D3DDECLUSAGE_POSITION:     return VS::Position;
    case D3DDECLUSAGE_POSITIONT:    return VS::Position;
    case D3DDECLUSAGE_NORMAL:       return VS::Normal;
    case D3DDECLUSAGE_TANGENT:      return VS::Tangent;
    case D3DDECLUSAGE_BINORMAL:     return VS::Binormal;
    case D3DDECLUSAGE_TEXCOORD:     return VS::TexCoord;
    case D3DDECLUSAGE_COLOR:        return VS::Color;
    case D3DDECLUSAGE_BLENDWEIGHT:  return VS::BlendWeights;
    case D3DDECLUSAGE_BLENDINDICES: return VS::BlendIndices;
    default:                        return VS::Custom;
    }
}

openripper::VertexLayout decode_decl(IDirect3DVertexDeclaration9* decl) {
    openripper::VertexLayout layout;
    UINT count = MAXD3DDECLLENGTH + 1;
    D3DVERTEXELEMENT9 elems[MAXD3DDECLLENGTH + 1];
    if (FAILED(decl->GetDeclaration(elems, &count))) return layout;

    for (UINT i = 0; i < count; ++i) {
        const auto& e = elems[i];
        if (e.Type == D3DDECLTYPE_UNUSED) break;

        AF fmt = d3dtype_to_af(e.Type);
        if (fmt == AF::Unknown) continue;

        openripper::VertexAttribute attr;
        attr.semantic       = d3dusage_to_sem(e.Usage);
        attr.semantic_index = e.UsageIndex;
        attr.format         = fmt;
        attr.offset         = e.Offset;
        attr.stream         = static_cast<std::uint8_t>(e.Stream);
        layout.attributes.push_back(attr);
    }
    return layout;
}

// Decode legacy FVF into a VertexLayout. Attributes appear in a fixed order;
// offsets are computed by accumulating element sizes.
openripper::VertexLayout decode_fvf(DWORD fvf) {
    openripper::VertexLayout layout;
    UINT offset = 0;
    const UINT tex_count = (fvf & D3DFVF_TEXCOUNT_MASK) >> D3DFVF_TEXCOUNT_SHIFT;

    auto add = [&](VS sem, UINT idx, AF fmt, UINT bytes) {
        openripper::VertexAttribute a;
        a.semantic       = sem;
        a.semantic_index = static_cast<std::uint8_t>(idx);
        a.format         = fmt;
        a.offset         = static_cast<std::uint16_t>(offset);
        a.stream         = 0;
        layout.attributes.push_back(a);
        offset += bytes;
    };

    if (fvf & D3DFVF_XYZRHW)     { add(VS::Position, 0, AF::Float32x4, 16); }
    else if (fvf & D3DFVF_XYZ)   { add(VS::Position, 0, AF::Float32x3, 12); }

    // Blend weights / indices (D3DFVF_XYZBn) — skip bytes; rarely needed for OBJ
    const DWORD pos_type = fvf & D3DFVF_POSITION_MASK;
    if (pos_type == D3DFVF_XYZB1) offset += 4;
    else if (pos_type == D3DFVF_XYZB2) offset += 8;
    else if (pos_type == D3DFVF_XYZB3) offset += 12;
    else if (pos_type == D3DFVF_XYZB4) offset += 16;
    else if (pos_type == D3DFVF_XYZB5) offset += 20;

    if (fvf & D3DFVF_NORMAL)   { add(VS::Normal,  0, AF::Float32x3, 12); }
    if (fvf & D3DFVF_PSIZE)    { offset += 4; } // point size — skip
    if (fvf & D3DFVF_DIFFUSE)  { add(VS::Color,   0, AF::UNorm8x4,   4); }
    if (fvf & D3DFVF_SPECULAR) { add(VS::Color,   1, AF::UNorm8x4,   4); }

    for (UINT t = 0; t < tex_count; ++t) {
        // Texture coordinate size modifier bits (2 bits per texture slot).
        const UINT size_bits = (fvf >> (16 + t * 2)) & 0x3;
        UINT components = 2;
        if      (size_bits == D3DFVF_TEXTUREFORMAT1) components = 1;
        else if (size_bits == D3DFVF_TEXTUREFORMAT3) components = 3;
        else if (size_bits == D3DFVF_TEXTUREFORMAT4) components = 4;
        AF fmt = (components == 1) ? AF::Float32x1
               : (components == 3) ? AF::Float32x3
               : (components == 4) ? AF::Float32x4
               : AF::Float32x2;
        add(VS::TexCoord, static_cast<std::uint8_t>(t), fmt, components * 4);
    }
    return layout;
}

// ---- Texture format helpers --------------------------------------------------

DXGI_FORMAT d3dfmt_to_dxgi(D3DFORMAT fmt) noexcept {
    switch (fmt) {
    case D3DFMT_DXT1:             return DXGI_FORMAT_BC1_UNORM;
    case D3DFMT_DXT2:             // premul-alpha DXT3 → treat as BC2
    case D3DFMT_DXT3:             return DXGI_FORMAT_BC2_UNORM;
    case D3DFMT_DXT4:             // premul-alpha DXT5 → treat as BC3
    case D3DFMT_DXT5:             return DXGI_FORMAT_BC3_UNORM;
    case D3DFMT_A8R8G8B8:         return DXGI_FORMAT_B8G8R8A8_UNORM;
    case D3DFMT_X8R8G8B8:         return DXGI_FORMAT_B8G8R8X8_UNORM;
    case D3DFMT_A8B8G8R8:         return DXGI_FORMAT_R8G8B8A8_UNORM;
    case D3DFMT_R5G6B5:           return DXGI_FORMAT_B5G6R5_UNORM;
    case D3DFMT_A1R5G5B5:         return DXGI_FORMAT_B5G5R5A1_UNORM;
    case D3DFMT_A8:               return DXGI_FORMAT_A8_UNORM;
    case D3DFMT_L8:               return DXGI_FORMAT_R8_UNORM;
    case D3DFMT_R32F:             return DXGI_FORMAT_R32_FLOAT;
    case D3DFMT_G32R32F:          return DXGI_FORMAT_R32G32_FLOAT;
    case D3DFMT_A32B32G32R32F:    return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case D3DFMT_R16F:             return DXGI_FORMAT_R16_FLOAT;
    case D3DFMT_G16R16F:          return DXGI_FORMAT_R16G16_FLOAT;
    case D3DFMT_A16B16G16R16F:    return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case D3DFMT_A16B16G16R16:     return DXGI_FORMAT_R16G16B16A16_UNORM;
    case D3DFMT_G16R16:           return DXGI_FORMAT_R16G16_UNORM;
    default:                      return DXGI_FORMAT_UNKNOWN;
    }
}

bool is_bc(D3DFORMAT fmt) noexcept {
    return fmt == D3DFMT_DXT1 || fmt == D3DFMT_DXT2 || fmt == D3DFMT_DXT3 ||
           fmt == D3DFMT_DXT4 || fmt == D3DFMT_DXT5;
}

UINT bc_bytes_per_block(D3DFORMAT fmt) noexcept {
    return (fmt == D3DFMT_DXT1) ? 8u : 16u;
}

UINT uncompressed_bpp(D3DFORMAT fmt) noexcept {
    switch (fmt) {
    case D3DFMT_A8R8G8B8: case D3DFMT_X8R8G8B8:
    case D3DFMT_A8B8G8R8: case D3DFMT_R32F:      return 4;
    case D3DFMT_R5G6B5:   case D3DFMT_A1R5G5B5:
    case D3DFMT_R16F:     case D3DFMT_G16R16:
    case D3DFMT_A8L8:                             return 2;
    case D3DFMT_L8:       case D3DFMT_A8:         return 1;
    case D3DFMT_G32R32F:  case D3DFMT_G16R16F:    return 8;
    case D3DFMT_A32B32G32R32F:                    return 16;
    case D3DFMT_A16B16G16R16F:
    case D3DFMT_A16B16G16R16:                     return 8;
    default:                                      return 0;
    }
}

// ---- Mesh capture internals --------------------------------------------------

// Lock a range of a vertex buffer and return the bytes, or empty on failure.
std::vector<std::byte> lock_vb(IDirect3DVertexBuffer9* vb,
                                UINT byte_offset, UINT byte_size) {
    D3DVERTEXBUFFER_DESC desc{};
    if (FAILED(vb->GetDesc(&desc))) return {};
    if (byte_offset >= desc.Size) return {};
    byte_size = std::min(byte_size, desc.Size - byte_offset);
    if (!byte_size) return {};

    void* ptr = nullptr;
    if (FAILED(vb->Lock(byte_offset, byte_size, &ptr, D3DLOCK_READONLY))) return {};
    std::vector<std::byte> out(byte_size);
    std::memcpy(out.data(), ptr, byte_size);
    vb->Unlock();
    return out;
}

// Lock a range of an index buffer and return the bytes, or empty on failure.
std::vector<std::byte> lock_ib(IDirect3DIndexBuffer9* ib,
                                UINT byte_offset, UINT byte_size) {
    D3DINDEXBUFFER_DESC desc{};
    if (FAILED(ib->GetDesc(&desc))) return {};
    if (byte_offset >= desc.Size) return {};
    byte_size = std::min(byte_size, desc.Size - byte_offset);
    if (!byte_size) return {};

    void* ptr = nullptr;
    if (FAILED(ib->Lock(byte_offset, byte_size, &ptr, D3DLOCK_READONLY))) return {};
    std::vector<std::byte> out(byte_size);
    std::memcpy(out.data(), ptr, byte_size);
    ib->Unlock();
    return out;
}

// ---- Texture readback --------------------------------------------------------

openripper::TextureSnapshot read_texture(IDirect3DTexture9* tex,
                                          std::uint32_t draw_id,
                                          std::uint32_t frame_id,
                                          DWORD stage) {
    openripper::TextureSnapshot snap;
    snap.name = std::format("tex_d{}s{}_f{:06}", draw_id, stage, frame_id);

    D3DSURFACE_DESC desc0{};
    if (FAILED(tex->GetLevelDesc(0, &desc0))) return snap;

    snap.width       = desc0.Width;
    snap.height      = desc0.Height;
    snap.depth       = 1;
    snap.mip_levels  = tex->GetLevelCount();
    snap.array_size  = 1;
    snap.native_format = static_cast<std::uint32_t>(d3dfmt_to_dxgi(desc0.Format));

    if (snap.native_format == DXGI_FORMAT_UNKNOWN) {
        OR_LOG_DEBUG("d3d9 tex: unsupported format {} - skipping", static_cast<int>(desc0.Format));
        return snap;
    }

    const bool bc    = is_bc(desc0.Format);
    const UINT bpp   = bc ? 0 : uncompressed_bpp(desc0.Format);
    const UINT bbpb  = bc ? bc_bytes_per_block(desc0.Format) : 0;

    snap.subresources.resize(snap.mip_levels);
    for (UINT m = 0; m < snap.mip_levels; ++m) {
        D3DSURFACE_DESC md{};
        if (FAILED(tex->GetLevelDesc(m, &md))) break;

        openripper::TextureSubresource& sub = snap.subresources[m];
        sub.mip_level = m;
        sub.width     = md.Width;
        sub.height    = md.Height;

        D3DLOCKED_RECT lr{};
        if (FAILED(tex->LockRect(m, &lr, nullptr, D3DLOCK_READONLY))) break;

        if (bc) {
            // Block-compressed: pitch is bytes per row of 4x4 blocks.
            const UINT block_rows = std::max(1u, (md.Height + 3u) / 4u);
            const UINT tight_row  = std::max(1u, (md.Width  + 3u) / 4u) * bbpb;
            sub.row_pitch = tight_row;
            sub.pixels.resize(static_cast<std::size_t>(tight_row) * block_rows);
            const auto* src = static_cast<const std::byte*>(lr.pBits);
            auto* dst = sub.pixels.data();
            for (UINT r = 0; r < block_rows; ++r) {
                std::memcpy(dst, src, tight_row);
                src += static_cast<UINT>(lr.Pitch);
                dst += tight_row;
            }
        } else if (bpp) {
            const UINT tight_row = md.Width * bpp;
            sub.row_pitch = tight_row;
            sub.pixels.resize(static_cast<std::size_t>(tight_row) * md.Height);
            const auto* src = static_cast<const std::byte*>(lr.pBits);
            auto* dst = sub.pixels.data();
            for (UINT r = 0; r < md.Height; ++r) {
                std::memcpy(dst, src, tight_row);
                src += static_cast<UINT>(lr.Pitch);
                dst += tight_row;
            }
        }

        tex->UnlockRect(m);
    }
    return snap;
}

} // namespace

// ---- Public API --------------------------------------------------------------

std::optional<openripper::MeshSnapshot>
capture_mesh(IDirect3DDevice9* dev,
             D3DPRIMITIVETYPE  prim_type,
             INT               base_vertex,
             UINT              min_vertex,
             UINT              num_vertices,
             UINT              start_index,
             UINT              prim_count,
             bool              indexed,
             std::uint32_t     draw_id,
             std::uint32_t     frame_id)
{
    // ---- Vertex layout ----
    openripper::VertexLayout layout;
    IDirect3DVertexDeclaration9* decl = nullptr;
    dev->GetVertexDeclaration(&decl);
    if (decl) {
        layout = decode_decl(decl);
        decl->Release();
    } else {
        DWORD fvf = 0;
        dev->GetFVF(&fvf);
        if (fvf) layout = decode_fvf(fvf);
    }

    if (layout.attributes.empty()) {
        OR_LOG_DEBUG("d3d9: draw {}: no vertex layout", draw_id);
        return std::nullopt;
    }

    // ---- Stream 0 vertex buffer ----
    IDirect3DVertexBuffer9* vb      = nullptr;
    UINT                    vb_off  = 0;
    UINT                    stride  = 0;
    dev->GetStreamSource(0, &vb, &vb_off, &stride);
    if (!vb || !stride) {
        if (vb) vb->Release();
        OR_LOG_DEBUG("d3d9: draw {}: no stream-0 VB", draw_id);
        return std::nullopt;
    }
    layout.stream_strides[0] = static_cast<std::uint16_t>(stride);

    // The vertex range we need:
    //   For indexed:    rows [base_vertex + min_vertex,  +num_vertices)  in the VB
    //   For non-indexed: rows [start_vertex_computed,    +num_vertices)  = same formula
    //   (caller sets base_vertex=start_vertex, min_vertex=0 for DrawPrimitive)
    const UINT first_row      = static_cast<UINT>(base_vertex) + min_vertex;
    const UINT vb_byte_start  = vb_off + first_row * stride;
    const UINT vb_byte_size   = num_vertices * stride;

    auto vb_data = lock_vb(vb, vb_byte_start, vb_byte_size);
    vb->Release();
    if (vb_data.empty()) {
        OR_LOG_DEBUG("d3d9: draw {}: VB lock failed", draw_id);
        return std::nullopt;
    }

    openripper::MeshSnapshot snap;
    snap.draw_id      = draw_id;
    snap.frame_id     = frame_id;
    snap.layout       = std::move(layout);
    snap.vertex_count = num_vertices;
    snap.topology     = d3dprim_to_topo(prim_type);
    snap.name         = std::format("mesh_draw{:04}_frame{:06}", draw_id, frame_id);
    snap.vertex_streams.resize(1);
    snap.vertex_streams[0] = std::move(vb_data);
    // base_vertex = -min_vertex: OBJ exporter adds base_vertex to raw indices,
    // bringing them from the raw IB range [min_vertex, min_vertex+count) back
    // to [0, count) relative to our sliced VB.
    snap.base_vertex  = -static_cast<std::int32_t>(min_vertex);
    snap.start_index  = 0;

    // ---- Index buffer (indexed draws only) ----
    if (indexed) {
        IDirect3DIndexBuffer9* ib = nullptr;
        dev->GetIndices(&ib);
        if (ib) {
            D3DINDEXBUFFER_DESC ib_desc{};
            ib->GetDesc(&ib_desc);
            const UINT idx_size    = (ib_desc.Format == D3DFMT_INDEX16) ? 2u : 4u;
            const UINT index_count = verts_from_prims(prim_type, prim_count);
            auto ib_data = lock_ib(ib, start_index * idx_size, index_count * idx_size);
            ib->Release();

            if (!ib_data.empty()) {
                snap.index_buffer = std::move(ib_data);
                snap.index_count  = index_count;
                snap.index_format = (ib_desc.Format == D3DFMT_INDEX16)
                                  ? openripper::IndexFormat::U16
                                  : openripper::IndexFormat::U32;
            }
        }
    }

    return snap;
}

std::optional<openripper::MeshSnapshot>
capture_mesh_up(D3DPRIMITIVETYPE  prim_type,
                UINT              prim_count,
                const void*       vdata,
                UINT              vstride,
                const void*       idata,
                D3DFORMAT         ifmt,
                UINT              num_vertices,
                std::uint32_t     draw_id,
                std::uint32_t     frame_id)
{
    if (!vdata || !vstride || !num_vertices) return std::nullopt;

    openripper::MeshSnapshot snap;
    snap.draw_id      = draw_id;
    snap.frame_id     = frame_id;
    snap.topology     = d3dprim_to_topo(prim_type);
    snap.vertex_count = num_vertices;
    snap.name         = std::format("mesh_draw{:04}_frame{:06}", draw_id, frame_id);
    snap.layout.stream_strides[0] = static_cast<std::uint16_t>(vstride);

    const std::size_t vb_size = static_cast<std::size_t>(num_vertices) * vstride;
    snap.vertex_streams.resize(1);
    snap.vertex_streams[0].resize(vb_size);
    std::memcpy(snap.vertex_streams[0].data(), vdata, vb_size);

    if (idata) {
        const UINT idx_size    = (ifmt == D3DFMT_INDEX16) ? 2u : 4u;
        const UINT index_count = verts_from_prims(prim_type, prim_count);
        snap.index_buffer.resize(static_cast<std::size_t>(index_count) * idx_size);
        std::memcpy(snap.index_buffer.data(), idata,
                    static_cast<std::size_t>(index_count) * idx_size);
        snap.index_count  = index_count;
        snap.index_format = (ifmt == D3DFMT_INDEX16)
                          ? openripper::IndexFormat::U16
                          : openripper::IndexFormat::U32;
    }

    // UP draws have no vertex declaration from the device; layout will be empty
    // (position-less OBJ is still useful for raw buffer inspection).
    return snap;
}

std::vector<CaptureTexResult>
capture_textures(IDirect3DDevice9* dev,
                 std::uint32_t     draw_id,
                 std::uint32_t     frame_id)
{
    std::vector<CaptureTexResult> out;
    for (DWORD stage = 0; stage < 8; ++stage) {
        IDirect3DBaseTexture9* base = nullptr;
        if (FAILED(dev->GetTexture(stage, &base)) || !base) continue;

        IDirect3DTexture9* tex2d = nullptr;
        if (SUCCEEDED(base->QueryInterface(IID_IDirect3DTexture9,
                                           reinterpret_cast<void**>(&tex2d)))) {
            // Dedup: same texture pointer seen earlier in this frame?
            if (g_dedup) {
                auto it = s_tex_dedup.find(tex2d);
                if (it != s_tex_dedup.end()) {
                    const auto& de = it->second;
                    OR_LOG_DEBUG("dedup: draw {} stage {} -> reusing {}",
                                 draw_id, stage, de.file);
                    CaptureTexResult r;
                    r.stage      = stage;
                    r.source_ptr = tex2d;
                    r.reuse_file = de.file;
                    r.reuse_fmt  = de.native_format;
                    r.reuse_w    = de.width;
                    r.reuse_h    = de.height;
                    r.reuse_mips = de.mip_levels;
                    out.push_back(std::move(r));
                    tex2d->Release();
                    base->Release();
                    continue;
                }
            }

            auto snap = read_texture(tex2d, draw_id, frame_id, stage);
            if (!snap.subresources.empty()) {
                CaptureTexResult r;
                r.stage      = stage;
                r.source_ptr = tex2d; // non-owning key; game holds its own ref
                r.snap       = std::move(snap);
                out.push_back(std::move(r));
            }
            tex2d->Release();
        }
        base->Release();
    }
    return out;
}

} // namespace openripper::backends::d3d9
