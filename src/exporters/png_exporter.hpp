// OpenRipper - src/exporters/png_exporter.hpp
//
// Write mip-0 of a TextureSnapshot as PNG via stb_image_write.
// Supported: R8G8B8A8_UNORM/SRGB, B8G8R8A8_UNORM/SRGB (R↔B swapped), R8_UNORM.
// Callers should fall back to write_dds() when this returns false.

#pragma once

#include "../core/types.hpp"
#include <filesystem>

namespace openripper::exporters {

// Returns false for unsupported formats or I/O failure; logs the reason.
bool write_png(const openripper::TextureSnapshot& tex,
               const std::filesystem::path& out_path);

} // namespace openripper::exporters
