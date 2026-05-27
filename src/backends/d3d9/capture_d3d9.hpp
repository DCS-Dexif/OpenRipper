#pragma once

#include "../../core/types.hpp"

#include <d3d9.h>

#include <cstdint>
#include <optional>
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

// Sample textures bound to stages 0-7 at the moment of a draw call.
// Returns (stage, snapshot) pairs for each successfully read texture.
std::vector<std::pair<DWORD, openripper::TextureSnapshot>>
capture_textures(IDirect3DDevice9* dev,
                 std::uint32_t     draw_id,
                 std::uint32_t     frame_id);

} // namespace openripper::backends::d3d9
