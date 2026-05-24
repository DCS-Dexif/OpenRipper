// OpenRipper - src/exporters/obj_exporter.cpp
//
// Wavefront OBJ exporter (Stage 2: positions + normals + UVs).
//
// Attribute extraction generalises over any semantic in the VertexLayout.
// Each attribute knows its stream slot, byte offset, and AttributeFormat, so
// the decoder just walks per-vertex records and picks out the relevant bytes.
//
// Supported source formats for float extraction:
//   Float32x{1..4}  - direct memcpy, drop components beyond what we need
//   Float16x{2,4}   - IEEE 754-2008 16-bit → 32-bit expansion (soft path;
//                     MSVC's _mm_cvtph_ps requires AVX so we avoid it here)
// All other formats are skipped with a warning; the mesh still exports
// without that channel rather than failing entirely.
//
// OBJ coordinate system note: OBJ UV origin is bottom-left (V increases
// upward). D3D11 textures use top-left (V increases downward). We flip V
// by writing (1 - v) so Blender/Maya see correct UVs without an extra step.

#include "obj_exporter.hpp"

#include "core/logger.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <vector>

namespace openripper::exporters {
namespace {

// ---- Float16 to Float32 conversion ----------------------------------------
// Soft implementation; avoids ISA-specific intrinsics so it builds on any
// MSVC target without extra /arch flags.
float f16_to_f32(std::uint16_t h) noexcept {
    // IEEE 754 half-precision layout: 1 sign, 5 exp, 10 mantissa.
    const std::uint32_t sign  = (h >> 15) & 1u;
    const std::uint32_t exp   = (h >> 10) & 0x1Fu;
    const std::uint32_t mant  = h & 0x3FFu;
    std::uint32_t f32 = 0;

    if (exp == 0) {
        // Subnormal or zero.
        if (mant == 0) {
            f32 = sign << 31;
        } else {
            // Normalise subnormal.
            std::uint32_t e = 127 - 14;
            std::uint32_t m = mant;
            while (!(m & (1u << 10))) { m <<= 1; e--; }
            f32 = (sign << 31) | (e << 23) | ((m & 0x3FFu) << 13);
        }
    } else if (exp == 31) {
        // Inf or NaN.
        f32 = (sign << 31) | (0xFFu << 23) | (mant << 13);
    } else {
        // Normalised.
        f32 = (sign << 31) | ((exp + (127 - 15)) << 23) | (mant << 13);
    }

    float result;
    std::memcpy(&result, &f32, sizeof(result));
    return result;
}

// ---- Generic attribute extraction ----------------------------------------
// Reads up to `want_components` floats from a single attribute for every
// vertex in its stream. Returns an empty vector on unsupported format or
// if the stream buffer is too small.
//
//  attr           - attribute descriptor (stream, offset, format)
//  vertex_streams - the snapshot's per-slot byte buffers
//  stride         - per-vertex stride for the attribute's stream
//  want_components - how many floats to read per vertex (1..4)
//
// The returned vector is laid out flat: [v0c0, v0c1, ..., v1c0, v1c1, ...]
// where each vertex contributes exactly want_components floats.
std::vector<float> extract_attribute(const VertexAttribute&                   attr,
                                      const std::vector<std::vector<std::byte>>& vertex_streams,
                                      std::uint16_t                             stride,
                                      std::size_t                               want_components)
{
    if (attr.stream >= vertex_streams.size()) return {};
    const auto& stream = vertex_streams[attr.stream];
    if (stride == 0 || stream.empty()) return {};

    const std::size_t vcount = stream.size() / stride;
    if (vcount == 0) return {};

    std::vector<float> out;
    out.reserve(vcount * want_components);

    const auto fmt = attr.format;

    // Helper: push min(available, want_components) components.
    auto push_f32 = [&](const std::byte* base, std::size_t avail) {
        const float* fp = reinterpret_cast<const float*>(base);
        const std::size_t n = std::min(avail, want_components);
        for (std::size_t c = 0; c < n; ++c) out.push_back(fp[c]);
        for (std::size_t c = n; c < want_components; ++c) out.push_back(0.0f);
    };

    auto push_f16 = [&](const std::byte* base, std::size_t avail) {
        const std::uint16_t* hp = reinterpret_cast<const std::uint16_t*>(base);
        const std::size_t n = std::min(avail, want_components);
        for (std::size_t c = 0; c < n; ++c) out.push_back(f16_to_f32(hp[c]));
        for (std::size_t c = n; c < want_components; ++c) out.push_back(0.0f);
    };

    for (std::size_t v = 0; v < vcount; ++v) {
        const std::byte* src = stream.data() + v * stride + attr.offset;

        switch (fmt) {
        case AttributeFormat::Float32x1: push_f32(src, 1); break;
        case AttributeFormat::Float32x2: push_f32(src, 2); break;
        case AttributeFormat::Float32x3: push_f32(src, 3); break;
        case AttributeFormat::Float32x4: push_f32(src, 4); break;
        case AttributeFormat::Float16x2: push_f16(src, 2); break;
        case AttributeFormat::Float16x4: push_f16(src, 4); break;
        default:
            // Unsupported format; return empty to signal to the caller.
            return {};
        }
    }
    return out;
}

// ---- Index helpers --------------------------------------------------------

// Total number of vertices in the flattened float array given components/vert.
inline std::size_t vertex_count_from(const std::vector<float>& v, std::size_t comps) {
    return comps ? v.size() / comps : 0;
}

} // namespace

bool write_obj(const MeshSnapshot& mesh, const std::filesystem::path& out_path) {
    // ---- Locate required Position attribute --------------------------------
    const VertexAttribute* pos_attr = nullptr;
    for (const auto& a : mesh.layout.attributes)
        if (a.semantic == VertexSemantic::Position) { pos_attr = &a; break; }

    if (!pos_attr) {
        OR_LOG_ERROR("obj exporter: no Position attribute in mesh '{}'", mesh.name);
        return false;
    }

    const std::uint16_t pos_stride = mesh.layout.stream_strides[pos_attr->stream];
    auto positions = extract_attribute(*pos_attr, mesh.vertex_streams, pos_stride, 3);
    if (positions.empty()) {
        OR_LOG_ERROR("obj exporter: Position format {:d} not extractable for mesh '{}'",
                     static_cast<int>(pos_attr->format), mesh.name);
        return false;
    }
    const std::size_t vcount = vertex_count_from(positions, 3);

    // ---- Locate optional Normal attribute ----------------------------------
    const VertexAttribute* nrm_attr = nullptr;
    for (const auto& a : mesh.layout.attributes)
        if (a.semantic == VertexSemantic::Normal && a.semantic_index == 0) { nrm_attr = &a; break; }

    std::vector<float> normals;
    if (nrm_attr) {
        const std::uint16_t stride = mesh.layout.stream_strides[nrm_attr->stream];
        normals = extract_attribute(*nrm_attr, mesh.vertex_streams, stride, 3);
        if (normals.empty())
            OR_LOG_WARN("obj exporter: Normal format not extractable for '{}' - skipping vn", mesh.name);
    }

    // ---- Locate optional TexCoord attribute (index 0) ----------------------
    const VertexAttribute* uv_attr = nullptr;
    for (const auto& a : mesh.layout.attributes)
        if (a.semantic == VertexSemantic::TexCoord && a.semantic_index == 0) { uv_attr = &a; break; }

    std::vector<float> uvs;
    if (uv_attr) {
        const std::uint16_t stride = mesh.layout.stream_strides[uv_attr->stream];
        uvs = extract_attribute(*uv_attr, mesh.vertex_streams, stride, 2);
        if (uvs.empty())
            OR_LOG_WARN("obj exporter: TexCoord format not extractable for '{}' - skipping vt", mesh.name);
    }

    const bool has_normals = !normals.empty() && vertex_count_from(normals, 3) == vcount;
    const bool has_uvs     = !uvs.empty()     && vertex_count_from(uvs, 2)    == vcount;

    // ---- Open output file --------------------------------------------------
    std::ofstream f(out_path);
    if (!f) {
        OR_LOG_ERROR("obj exporter: failed to open '{}' for writing", out_path.string());
        return false;
    }

    f << "# OpenRipper OBJ export\n";
    f << "# mesh: " << mesh.name
      << "  frame=" << mesh.frame_id
      << " draw="   << mesh.draw_id  << "\n";
    if (has_normals) f << "# normals: yes\n";
    if (has_uvs)     f << "# uvs: yes\n";
    f << "o " << (mesh.name.empty() ? "mesh" : mesh.name) << "\n";

    // ---- Vertex positions (v) ---------------------------------------------
    for (std::size_t i = 0; i < vcount; ++i) {
        f << "v " << positions[i * 3 + 0]
          << ' '  << positions[i * 3 + 1]
          << ' '  << positions[i * 3 + 2] << '\n';
    }

    // ---- Texture coordinates (vt) -----------------------------------------
    // Flip V: OBJ convention is bottom-left origin; D3D texture is top-left.
    if (has_uvs) {
        for (std::size_t i = 0; i < vcount; ++i) {
            f << "vt " << uvs[i * 2 + 0]
              << ' '   << (1.0f - uvs[i * 2 + 1]) << '\n';
        }
    }

    // ---- Normals (vn) -----------------------------------------------------
    if (has_normals) {
        for (std::size_t i = 0; i < vcount; ++i) {
            f << "vn " << normals[i * 3 + 0]
              << ' '   << normals[i * 3 + 1]
              << ' '   << normals[i * 3 + 2] << '\n';
        }
    }

    // ---- Faces (f) --------------------------------------------------------
    // OBJ indices are 1-based. Face vertex tokens vary depending on what
    // channels are available:
    //   positions only  →  "f a b c"
    //   positions + UV  →  "f a/a b/b c/c"
    //   positions + N   →  "f a//a b//b c//c"
    //   all three       →  "f a/a/a b/b/b c/c/c"
    // Because we emit exactly vcount vt/vn entries (same order as v), the
    // vertex index serves as the UV and normal index too.
    //
    // Only TriangleList topology produces correct faces here. Other topologies
    // are captured into MeshSnapshot but the face decoder below assumes
    // consecutive triples → triangles. Non-triangleList draws are rare in
    // modern pipelines and will be handled per-topology in a future stage.
    auto emit_face_vertex = [&](std::uint32_t zero_based) {
        const auto i = zero_based + 1;   // convert to 1-based OBJ index
        if (has_uvs && has_normals)      f << i << '/' << i << '/' << i;
        else if (has_uvs)                f << i << '/' << i;
        else if (has_normals)            f << i << "//" << i;
        else                             f << i;
    };

    auto emit_triangle = [&](std::uint32_t a, std::uint32_t b, std::uint32_t c) {
        f << 'f' << ' ';
        emit_face_vertex(a); f << ' ';
        emit_face_vertex(b); f << ' ';
        emit_face_vertex(c); f << '\n';
    };

    if (mesh.index_format == IndexFormat::U16) {
        const auto* idx = reinterpret_cast<const std::uint16_t*>(mesh.index_buffer.data());
        for (std::uint32_t i = 0; i + 2 < mesh.index_count; i += 3)
            emit_triangle(idx[i], idx[i + 1], idx[i + 2]);
    } else if (mesh.index_format == IndexFormat::U32) {
        const auto* idx = reinterpret_cast<const std::uint32_t*>(mesh.index_buffer.data());
        for (std::uint32_t i = 0; i + 2 < mesh.index_count; i += 3)
            emit_triangle(idx[i], idx[i + 1], idx[i + 2]);
    } else {
        // Non-indexed: consecutive vertices form triangles.
        const auto n = static_cast<std::uint32_t>(vcount);
        for (std::uint32_t i = 0; i + 2 < n; i += 3)
            emit_triangle(i, i + 1, i + 2);
    }

    OR_LOG_INFO("obj exporter: wrote '{}' ({} verts, {} tris{}{})",
                out_path.filename().string(),
                vcount,
                mesh.index_format != IndexFormat::None
                    ? mesh.index_count / 3
                    : vcount / 3,
                has_uvs     ? " +uvs"     : "",
                has_normals ? " +normals" : "");
    return true;
}

} // namespace openripper::exporters
