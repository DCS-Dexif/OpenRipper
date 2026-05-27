#pragma once

#include "../../core/types.hpp"

#include <d3d9.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace openripper::backends::d3d9 {

// Capture from a real VB/IB (DrawPrimitive / DrawIndexedPrimitive).
// For DrawPrimitive: base_vertex=0, min_vertex=0, start_index=0, indexed=false.
// num_vertices must be pre-computed from prim_type + prim_count by the caller.
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
             std::uint32_t     frame_id);

// Capture from caller-supplied user-pointer data (DrawPrimitiveUP /
// DrawIndexedPrimitiveUP). idata may be null for non-indexed UP draws.
std::optional<openripper::MeshSnapshot>
capture_mesh_up(D3DPRIMITIVETYPE  prim_type,
                UINT              prim_count,
                const void*       vdata,
                UINT              vstride,
                const void*       idata,   // null → non-indexed
                D3DFORMAT         ifmt,
                UINT              num_vertices,
                std::uint32_t     draw_id,
                std::uint32_t     frame_id);

// Result from capture_textures. When reuse_file is empty, snap holds pixel data
// and source_ptr is the non-owning IDirect3DTexture9* for dedup registration.
// When reuse_file is non-empty, skip write and reuse the cached filename.
struct CaptureTexResult {
    DWORD                       stage;
    openripper::TextureSnapshot snap;           // pixel data if reuse_file.empty()
    IDirect3DTexture9*          source_ptr{nullptr}; // non-owning dedup key
    std::string                 reuse_file;
    std::uint32_t               reuse_fmt{0};
    std::uint32_t               reuse_w{0};
    std::uint32_t               reuse_h{0};
    std::uint32_t               reuse_mips{0};
};

// Sample textures bound to stages 0-7 at the moment of a draw call.
// When g_dedup is enabled, stages that reuse the same IDirect3DTexture9* seen
// earlier in the frame are returned with reuse_file set (no GPU readback).
std::vector<CaptureTexResult>
capture_textures(IDirect3DDevice9* dev,
                 std::uint32_t     draw_id,
                 std::uint32_t     frame_id);

// Clear the per-frame dedup map. Call at the start of each capture frame.
void dedup_begin_frame();

// Register a texture pointer -> written filename after a successful write.
void dedup_tex_register(IDirect3DTexture9*                 ptr,
                        std::string_view                   filename,
                        const openripper::TextureSnapshot& snap_meta);

} // namespace openripper::backends::d3d9
