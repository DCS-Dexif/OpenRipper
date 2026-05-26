// OpenRipper - src/exporters/material_exporter.hpp
//
// Per-frame material manifest: maps each draw to its OBJ file and the texture
// files captured from its PS SRV slots. Written as a hand-crafted JSON file.

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace openripper::exporters {

struct DrawMaterialRecord {
    std::uint32_t draw_id   = 0;
    std::string   mesh_file;          // basename, e.g. "frame000120_draw00000.obj"

    struct Tex {
        std::uint32_t slot          = 0;
        std::string   file;           // basename, e.g. "frame000120_draw00000_ps_t0.png"
        std::uint32_t native_format  = 0;
        std::uint32_t width         = 0;
        std::uint32_t height        = 0;
        std::uint32_t mips          = 1;
    };
    std::vector<Tex> ps_textures;
};

// Write "frame######_materials.json" listing draw → mesh → texture mappings.
// Returns false on I/O failure.
bool write_material_manifest(std::uint32_t                           frame_id,
                             const std::vector<DrawMaterialRecord>&  draws,
                             const std::filesystem::path&            out_path);

} // namespace openripper::exporters
