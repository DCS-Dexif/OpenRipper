// OpenRipper - src/exporters/dds_exporter.cpp
//
// Hand-rolled DDS writer using the DX10-extended header format.
// No external library; structs derived from the public Microsoft DDS spec.

#include "dds_exporter.hpp"
#include "../core/logger.hpp"

#include <cstdint>
#include <fstream>

namespace openripper::exporters {
namespace {

static constexpr std::uint32_t k_magic = 0x20534444u; // 'DDS '

// DDS_HEADER.dwFlags bits
static constexpr std::uint32_t DDSD_CAPS        = 0x00000001u;
static constexpr std::uint32_t DDSD_HEIGHT      = 0x00000002u;
static constexpr std::uint32_t DDSD_WIDTH       = 0x00000004u;
static constexpr std::uint32_t DDSD_PITCH       = 0x00000008u;
static constexpr std::uint32_t DDSD_PIXELFORMAT = 0x00001000u;
static constexpr std::uint32_t DDSD_MIPMAPCOUNT = 0x00020000u;
static constexpr std::uint32_t DDSD_LINEARSIZE  = 0x00080000u;

// DDS_PIXELFORMAT.dwFlags
static constexpr std::uint32_t DDPF_FOURCC = 0x00000004u;

// FourCC indicating DX10 extended header
static constexpr std::uint32_t FOURCC_DX10 = 0x30315844u; // 'DX10'

// DDS_HEADER.dwCaps bits
static constexpr std::uint32_t DDSCAPS_COMPLEX = 0x00000008u;
static constexpr std::uint32_t DDSCAPS_MIPMAP  = 0x00400000u;
static constexpr std::uint32_t DDSCAPS_TEXTURE = 0x00001000u;

// D3D10_RESOURCE_DIMENSION_TEXTURE2D
static constexpr std::uint32_t DIM_TEXTURE2D = 3u;

// BC1_TYPELESS(70)..BC7_UNORM_SRGB(99) form a contiguous block in DXGI_FORMAT.
bool is_block_compressed(std::uint32_t fmt) noexcept {
    return (fmt >= 70u && fmt <= 99u);
}

#pragma pack(push, 1)
struct DdsPixelFormat {
    std::uint32_t size;           // must be 32
    std::uint32_t flags;
    std::uint32_t four_cc;
    std::uint32_t rgb_bit_count;
    std::uint32_t r_bit_mask;
    std::uint32_t g_bit_mask;
    std::uint32_t b_bit_mask;
    std::uint32_t a_bit_mask;
};
static_assert(sizeof(DdsPixelFormat) == 32);

struct DdsHeader {
    std::uint32_t  size;          // must be 124
    std::uint32_t  flags;
    std::uint32_t  height;
    std::uint32_t  width;
    std::uint32_t  pitch_or_linear_size;
    std::uint32_t  depth;
    std::uint32_t  mip_map_count;
    std::uint32_t  reserved1[11];
    DdsPixelFormat ddspf;
    std::uint32_t  caps;
    std::uint32_t  caps2;
    std::uint32_t  caps3;
    std::uint32_t  caps4;
    std::uint32_t  reserved2;
};
static_assert(sizeof(DdsHeader) == 124);

struct DdsHeaderDxt10 {
    std::uint32_t dxgi_format;
    std::uint32_t resource_dimension;
    std::uint32_t misc_flag;
    std::uint32_t array_size;
    std::uint32_t misc_flags2;
};
static_assert(sizeof(DdsHeaderDxt10) == 20);
#pragma pack(pop)

} // namespace

bool write_dds(const openripper::TextureSnapshot& tex,
               const std::filesystem::path& out_path)
{
    if (tex.subresources.empty()) {
        OR_LOG_ERROR("dds: no subresources in snapshot '{}'", tex.name);
        return false;
    }

    std::ofstream f(out_path, std::ios::binary);
    if (!f) {
        OR_LOG_ERROR("dds: cannot open '{}' for writing", out_path.string());
        return false;
    }

    const bool is_bc    = is_block_compressed(tex.native_format);
    const bool has_mips = (tex.mip_levels > 1);

    std::uint32_t flags = DDSD_CAPS | DDSD_HEIGHT | DDSD_WIDTH
                        | DDSD_PIXELFORMAT | DDSD_MIPMAPCOUNT;
    flags |= (is_bc ? DDSD_LINEARSIZE : DDSD_PITCH);

    std::uint32_t caps = DDSCAPS_TEXTURE;
    if (has_mips) caps |= DDSCAPS_COMPLEX | DDSCAPS_MIPMAP;

    DdsPixelFormat ddspf{};
    ddspf.size    = 32;
    ddspf.flags   = DDPF_FOURCC;
    ddspf.four_cc = FOURCC_DX10;

    DdsHeader hdr{};
    hdr.size                 = 124;
    hdr.flags                = flags;
    hdr.height               = tex.height;
    hdr.width                = tex.width;
    hdr.pitch_or_linear_size = static_cast<std::uint32_t>(tex.subresources[0].pixels.size());
    hdr.depth                = 0;
    hdr.mip_map_count        = tex.mip_levels;
    hdr.ddspf                = ddspf;
    hdr.caps                 = caps;

    DdsHeaderDxt10 dx10{};
    dx10.dxgi_format        = tex.native_format;
    dx10.resource_dimension = DIM_TEXTURE2D;
    dx10.misc_flag          = 0;
    dx10.array_size         = 1;
    dx10.misc_flags2        = 0;

    f.write(reinterpret_cast<const char*>(&k_magic), 4);
    f.write(reinterpret_cast<const char*>(&hdr),     sizeof(hdr));
    f.write(reinterpret_cast<const char*>(&dx10),    sizeof(dx10));

    for (const auto& sub : tex.subresources) {
        f.write(reinterpret_cast<const char*>(sub.pixels.data()),
                static_cast<std::streamsize>(sub.pixels.size()));
    }

    if (!f) {
        OR_LOG_ERROR("dds: write failed for '{}'", out_path.string());
        return false;
    }

    OR_LOG_INFO("dds: wrote '{}' ({}x{}, {} mip{}, dxgi={})",
                out_path.filename().string(),
                tex.width, tex.height,
                tex.mip_levels, tex.mip_levels > 1 ? "s" : "",
                tex.native_format);
    return true;
}

} // namespace openripper::exporters
