// OpenRipper - src/exporters/dds_exporter.hpp
//
// Write a Microsoft DDS file (DX10-extended header) from a TextureSnapshot.
// Preserves block-compressed formats (BC1-BC7) byte-for-byte.

#pragma once

#include "../core/types.hpp"
#include <filesystem>

namespace openripper::exporters {

// Returns false on unsupported format or I/O failure; logs the reason.
bool write_dds(const openripper::TextureSnapshot& tex,
               const std::filesystem::path& out_path);

} // namespace openripper::exporters
