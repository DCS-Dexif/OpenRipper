// OpenRipper - src/exporters/obj_exporter.cpp

#include "obj_exporter.hpp"

#include "core/logger.hpp"

#include <array>
#include <cstring>
#include <fstream>
#include <vector>

namespace openripper::exporters {
namespace {

// Walk the layout for a Position attribute, then unpack one float3 per
// vertex from the matching stream. Stage 1 supports Float32x3 / Float32x4
// in stream 0 only; richer extraction comes when live capture lands.
bool extract_positions(const MeshSnapshot& m,
                       std::vector<std::array<float, 3>>& out) {
    if (m.vertex_streams.empty()) return false;

    const VertexAttribute* pos = nullptr;
    for (const auto& a : m.layout.attributes) {
        if (a.semantic == VertexSemantic::Position) { pos = &a; break; }
    }
    if (!pos) return false;

    if (pos->format != AttributeFormat::Float32x3 &&
        pos->format != AttributeFormat::Float32x4) {
        OR_LOG_WARN("obj exporter: unsupported position format (Stage 1 limit)");
        return false;
    }
    if (pos->stream >= m.vertex_streams.size()) return false;

    const auto& stream = m.vertex_streams[pos->stream];
    const auto  stride = m.layout.stream_strides[pos->stream];
    if (stride == 0) return false;

    const std::size_t vcount = stream.size() / stride;
    out.resize(vcount);
    for (std::size_t i = 0; i < vcount; ++i) {
        std::array<float, 3> p{};
        std::memcpy(p.data(),
                    stream.data() + i * stride + pos->offset,
                    sizeof(p));
        out[i] = p;
    }
    return true;
}

} // namespace

bool write_obj(const MeshSnapshot& mesh, const std::filesystem::path& out_path) {
    std::vector<std::array<float, 3>> positions;
    if (!extract_positions(mesh, positions)) {
        OR_LOG_ERROR("obj exporter: no extractable Position attribute for mesh '{}'",
                     mesh.name);
        return false;
    }

    std::ofstream f(out_path);
    if (!f) {
        OR_LOG_ERROR("obj exporter: failed to open '{}' for writing",
                     out_path.string());
        return false;
    }

    f << "# OpenRipper OBJ export (Stage 1: positions + indices only)\n";
    f << "# mesh: " << mesh.name
      << "  frame=" << mesh.frame_id
      << " draw="   << mesh.draw_id  << "\n";
    f << "o " << (mesh.name.empty() ? "mesh" : mesh.name) << "\n";

    for (const auto& p : positions) {
        f << "v " << p[0] << ' ' << p[1] << ' ' << p[2] << '\n';
    }

    // OBJ indices are 1-based. Stage 1 assumes triangle lists; other
    // topologies will arrive with the per-draw topology capture in Stage 2.
    auto emit_triangle = [&](std::uint32_t a, std::uint32_t b, std::uint32_t c) {
        f << "f " << (a + 1) << ' ' << (b + 1) << ' ' << (c + 1) << '\n';
    };

    if (mesh.index_format == IndexFormat::U16) {
        const auto* idx = reinterpret_cast<const std::uint16_t*>(mesh.index_buffer.data());
        for (std::uint32_t i = 0; i + 2 < mesh.index_count; i += 3) {
            emit_triangle(idx[i], idx[i + 1], idx[i + 2]);
        }
    } else if (mesh.index_format == IndexFormat::U32) {
        const auto* idx = reinterpret_cast<const std::uint32_t*>(mesh.index_buffer.data());
        for (std::uint32_t i = 0; i + 2 < mesh.index_count; i += 3) {
            emit_triangle(idx[i], idx[i + 1], idx[i + 2]);
        }
    } else {
        // Non-indexed: treat consecutive vertices as a triangle list.
        const auto n = static_cast<std::uint32_t>(positions.size());
        for (std::uint32_t i = 0; i + 2 < n; i += 3) {
            emit_triangle(i, i + 1, i + 2);
        }
    }

    return true;
}

} // namespace openripper::exporters
