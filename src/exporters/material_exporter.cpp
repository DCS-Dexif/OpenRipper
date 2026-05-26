// OpenRipper - src/exporters/material_exporter.cpp

#include "material_exporter.hpp"
#include "../core/logger.hpp"

#include <fstream>

namespace openripper::exporters {

bool write_material_manifest(std::uint32_t                           frame_id,
                             const std::vector<DrawMaterialRecord>&  draws,
                             const std::filesystem::path&            out_path)
{
    std::ofstream f(out_path);
    if (!f) {
        OR_LOG_ERROR("material: cannot open '{}' for writing", out_path.string());
        return false;
    }

    f << "{\n";
    f << "  \"frame\": " << frame_id << ",\n";
    f << "  \"draws\": [\n";

    for (std::size_t d = 0; d < draws.size(); ++d) {
        const auto& dr = draws[d];
        f << "    {\n";
        f << "      \"draw_id\": " << dr.draw_id << ",\n";
        f << "      \"mesh\": \"" << dr.mesh_file << "\",\n";
        f << "      \"ps_textures\": [\n";

        for (std::size_t t = 0; t < dr.ps_textures.size(); ++t) {
            const auto& tx = dr.ps_textures[t];
            f << "        {\n";
            f << "          \"slot\": "    << tx.slot          << ",\n";
            f << "          \"file\": \""  << tx.file          << "\",\n";
            f << "          \"format\": "  << tx.native_format << ",\n";
            f << "          \"width\": "   << tx.width         << ",\n";
            f << "          \"height\": "  << tx.height        << ",\n";
            f << "          \"mips\": "    << tx.mips          << "\n";
            f << "        }";
            if (t + 1 < dr.ps_textures.size()) f << ',';
            f << '\n';
        }

        f << "      ]\n";
        f << "    }";
        if (d + 1 < draws.size()) f << ',';
        f << '\n';
    }

    f << "  ]\n";
    f << "}\n";

    if (!f) {
        OR_LOG_ERROR("material: write error for '{}'", out_path.string());
        return false;
    }

    OR_LOG_INFO("material: wrote '{}' ({} draw records)", out_path.filename().string(), draws.size());
    return true;
}

} // namespace openripper::exporters
