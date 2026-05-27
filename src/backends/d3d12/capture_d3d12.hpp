// OpenRipper - src/backends/d3d12/capture_d3d12.hpp
//
// GPU->CPU readback for the D3D12 backend.
//
// Design:
//   - UPLOAD heap buffers are copied immediately at draw time (CPU-visible;
//     Map/Unmap, no GPU commands needed).
//   - DEFAULT heap buffers and textures are deferred: at the end of each
//     capture frame (in hooked_present), GPU work is fence-drained, then a
//     dedicated command list issues barriers + CopyBufferRegion/CopyTextureRegion
//     on the game's direct command queue, followed by another fence-wait and
//     Map on READBACK heap buffers.
//
// capture_readback_defaults():
//   Called from hooked_present after all draw records have been accumulated.
//   For each DrawRecord with non-empty vb_resources/ib_resource/textures,
//   performs GPU readback and fills in the immediate_vb/immediate_ib/pixel
//   fields so the caller can then produce MeshSnapshot/TextureSnapshot
//   independently.
//
// make_mesh_snapshot():
//   Translates a fully-populated DrawRecord into a MeshSnapshot by decoding
//   the vertex layout from the PSO registry and applying the draw parameters.
//
// make_texture_snapshot():
//   Translates a GPU-readback texture resource + its metadata into a
//   TextureSnapshot (mip chain for array slice 0).

#pragma once

#include "state_d3d12.hpp"
#include "../../core/types.hpp"

#include <d3d12.h>
#include <optional>
#include <vector>

namespace openripper::backends::d3d12 {

// Called once, lazily, to create the readback infrastructure.
// Requires g_device and g_game_queue to be set (captured from hooks).
bool init_readback_infra(ID3D12Device* device);

// Release all readback resources (called from remove_hooks).
void shutdown_readback_infra();

// For each DrawRecord in `recs` that has DEFAULT-heap VBs/IBs/textures:
//   - Fence-drain the game queue (called ONCE before iterating).
//   - Submit copy commands on the game queue, fence-drain again.
//   - Map readback buffers and fill immediate_vb/immediate_ib.
//   - For textures: map and fill TextureSnapshot data.
// Records with only UPLOAD-heap data (immediate_vb already populated) are
// left untouched.
//
// `texture_snapshots` receives one entry per texture found, paired with the
// DrawRecord index and the SRV slot number.
struct TextureReadback {
    std::uint32_t draw_idx;
    std::uint32_t srv_slot;
    openripper::TextureSnapshot snap;
    ID3D12Resource* source_resource{nullptr}; // non-owning; for per-frame dedup
};
void capture_readback_defaults(std::vector<DrawRecord>& recs,
                                ID3D12Device* device,
                                ID3D12CommandQueue* queue,
                                std::vector<TextureReadback>& tex_out);

// Build a MeshSnapshot from a DrawRecord whose immediate VB/IB data is ready.
std::optional<openripper::MeshSnapshot>
make_mesh_snapshot(const DrawRecord& rec, std::uint32_t draw_id, std::uint32_t frame_id);

} // namespace openripper::backends::d3d12
