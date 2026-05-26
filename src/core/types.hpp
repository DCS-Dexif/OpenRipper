// OpenRipper - src/core/types.hpp
//
// API-agnostic capture types. Every backend translates its native vertex /
// texture descriptors into these structures so exporters never need to know
// whether the data came from D3D11, Vulkan, GL, or a replay file.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace openripper {

// ---- Vertex layout ----------------------------------------------------------

enum class VertexSemantic : std::uint8_t {
    Position,
    Normal,
    Tangent,
    Binormal,
    Color,
    TexCoord,
    BlendIndices,
    BlendWeights,
    PointSize,
    Fog,
    Custom,
};

enum class AttributeFormat : std::uint8_t {
    Unknown,

    Float32x1, Float32x2, Float32x3, Float32x4,
    Float16x2, Float16x4,

    UInt32x1,  UInt32x2,  UInt32x3,  UInt32x4,
    UInt16x2,  UInt16x4,
    UInt8x4,

    SNorm16x2, SNorm16x4,
    UNorm8x4,
};

struct VertexAttribute {
    VertexSemantic  semantic       = VertexSemantic::Custom;
    std::uint8_t    semantic_index = 0;        // e.g. TexCoord 0..N
    AttributeFormat format         = AttributeFormat::Unknown;
    std::uint16_t   offset         = 0;        // byte offset within vertex
    std::uint8_t    stream         = 0;        // input slot / binding
};

struct VertexLayout {
    std::vector<VertexAttribute>   attributes;
    std::array<std::uint16_t, 16>  stream_strides{}; // byte stride per slot
};

// ---- Mesh -------------------------------------------------------------------

enum class IndexFormat : std::uint8_t {
    None,
    U16,
    U32,
};

enum class PrimitiveTopology : std::uint8_t {
    Unknown,
    PointList,
    LineList,
    LineStrip,
    TriangleList,
    TriangleStrip,
};

struct MeshSnapshot {
    std::string                          name;
    VertexLayout                         layout;

    // One raw byte buffer per used input slot. Indexed via VertexAttribute::stream.
    std::vector<std::vector<std::byte>>  vertex_streams;

    std::vector<std::byte>               index_buffer;
    IndexFormat                          index_format = IndexFormat::None;
    PrimitiveTopology                    topology     = PrimitiveTopology::TriangleList;

    std::uint32_t                        vertex_count = 0;
    std::uint32_t                        index_count  = 0;

    // Correlation metadata - useful for filenames and manifest sidecars.
    std::uint32_t                        draw_id  = 0;
    std::uint32_t                        frame_id = 0;
};

// ---- Texture ----------------------------------------------------------------

struct TextureSubresource {
    std::uint32_t           mip_level = 0;
    std::uint32_t           width     = 0;   // dimension at this mip level
    std::uint32_t           height    = 0;
    std::uint32_t           row_pitch = 0;   // bytes per row, tightly packed (no API padding)
    std::vector<std::byte>  pixels;
};

struct TextureSnapshot {
    std::string             name;
    std::uint32_t           width       = 0;
    std::uint32_t           height      = 0;
    std::uint32_t           depth       = 1;
    std::uint32_t           mip_levels  = 1;
    std::uint32_t           array_size  = 1;

    // Raw API format token, e.g. a DXGI_FORMAT value. The backend that
    // produced the snapshot is responsible for tagging it; exporters use it
    // to decide between DDS and PNG output.
    std::uint32_t           native_format = 0;

    // Mip chain for array slice 0. subresources[0] = top mip, [N-1] = smallest.
    // Stage 3.1 will expand to per-slice for cubemap / texture array support.
    std::vector<TextureSubresource> subresources;
};

} // namespace openripper
