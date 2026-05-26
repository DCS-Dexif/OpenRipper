// OpenRipper - src/exporters/png_exporter.cpp
//
// PNG writer via stb_image_write (public-domain / MIT, GPLv3-compatible).
// STB_IMAGE_WRITE_IMPLEMENTATION is defined here so the implementation lands
// in exactly one translation unit.

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include "png_exporter.hpp"
#include "../core/logger.hpp"

#include <cstdint>
#include <vector>

namespace openripper::exporters {

bool write_png(const openripper::TextureSnapshot& tex,
               const std::filesystem::path& out_path)
{
    if (tex.subresources.empty()) {
        OR_LOG_WARN("png: no subresources in '{}'", tex.name);
        return false;
    }
    const auto& mip0 = tex.subresources[0];

    // Map DXGI format → stb component count; flag B8G8R8A8 variants for R↔B swap.
    int  comps   = 0;
    bool swap_rb = false;

    switch (tex.native_format) {
    case 28: // DXGI_FORMAT_R8G8B8A8_UNORM
    case 29: // DXGI_FORMAT_R8G8B8A8_UNORM_SRGB
        comps = 4;
        break;
    case 87: // DXGI_FORMAT_B8G8R8A8_UNORM
    case 91: // DXGI_FORMAT_B8G8R8A8_UNORM_SRGB
        comps   = 4;
        swap_rb = true;
        break;
    case 61: // DXGI_FORMAT_R8_UNORM
        comps = 1;
        break;
    default:
        OR_LOG_WARN("png: unsupported format {} in '{}' - caller should use DDS",
                    tex.native_format, tex.name);
        return false;
    }

    const std::uint8_t* data = reinterpret_cast<const std::uint8_t*>(mip0.pixels.data());
    std::vector<std::uint8_t> swapped;

    if (swap_rb) {
        swapped.assign(
            reinterpret_cast<const std::uint8_t*>(mip0.pixels.data()),
            reinterpret_cast<const std::uint8_t*>(mip0.pixels.data()) + mip0.pixels.size());
        for (std::size_t i = 0; i + 3 < swapped.size(); i += 4)
            std::swap(swapped[i], swapped[i + 2]);  // B ↔ R
        data = swapped.data();
    }

    const int w      = static_cast<int>(mip0.width);
    const int h      = static_cast<int>(mip0.height);
    const int stride = static_cast<int>(mip0.row_pitch);

    if (!stbi_write_png(out_path.string().c_str(), w, h, comps, data, stride)) {
        OR_LOG_ERROR("png: stbi_write_png failed for '{}'", out_path.string());
        return false;
    }

    OR_LOG_INFO("png: wrote '{}' ({}x{}, dxgi={})",
                out_path.filename().string(), w, h, tex.native_format);
    return true;
}

} // namespace openripper::exporters
