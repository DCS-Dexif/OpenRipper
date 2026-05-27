// OpenRipper - src/backends/d3d11/capture_d3d11.hpp
//
// Per-draw capture entry point for the D3D11 backend (Stages 2–3).
// Called from each Draw* hook when g_capture_active is true.

#pragma once

#include "../../core/types.hpp"

#include <d3d11.h>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace openripper::backends::d3d11 {

// Snapshot all IA state on an *immediate* D3D11 device context, GPU->CPU copy
// every bound vertex/index buffer via D3D11_USAGE_STAGING, decode the input
// layout from the registry built by the CreateInputLayout hook, and return a
// fully-populated MeshSnapshot.
//
// Returns std::nullopt when capture is not possible:
//   - ctx is a deferred context (staged readback is illegal there)
//   - no input layout is bound
//   - the input layout was not registered (created before hooks installed)
//   - a D3D11 error occurs during CreateBuffer / CopyResource / Map
//
// Parameters mirror the D3D11 Draw* family; unused slots are 0 / -1:
//   index_count  -- IndexCount arg; 0 means non-indexed draw
//   vertex_count -- VertexCount (non-indexed) or VertexCountPerInstance
//   start_index  -- StartIndexLocation (0 for non-indexed)
//   base_vertex  -- BaseVertexLocation (may be negative in D3D11)
//   draw_id      -- 0-based sequential index within the capture frame
//   frame_id     -- absolute frame counter from g_frame_counter
std::optional<openripper::MeshSnapshot> capture_draw(ID3D11DeviceContext* ctx,
                                                std::uint32_t        index_count,
                                                std::uint32_t        vertex_count,
                                                std::uint32_t        start_index,
                                                std::int32_t         base_vertex,
                                                std::uint32_t        draw_id,
                                                std::uint32_t        frame_id);

// Result from capture_pixel_textures. When reuse_file is empty the snap holds
// freshly read pixel data and source_ptr is the non-owning texture pointer to
// register in the dedup map after the caller decides the output filename.
// When reuse_file is non-empty the texture was already written earlier in this
// frame; the caller should reuse the filename instead of writing again.
struct CaptureTexResult {
    std::uint32_t               slot;
    openripper::TextureSnapshot snap;           // pixel data if reuse_file.empty()
    ID3D11Texture2D*            source_ptr{nullptr}; // non-owning; valid for dedup registration
    std::string                 reuse_file;     // non-empty = already on disk
    // Metadata fields for the material record when reuse_file is non-empty
    std::uint32_t               reuse_fmt{0};
    std::uint32_t               reuse_w{0};
    std::uint32_t               reuse_h{0};
    std::uint32_t               reuse_mips{0};
};

// Walk all 128 PS SRV slots, stage each backing ID3D11Texture2D to CPU (full
// mip chain, slice 0), and return the results.
// Only D3D11_SRV_DIMENSION_TEXTURE2D views are captured; others are skipped.
// When g_dedup is true, textures whose ID3D11Texture2D* was already seen this
// frame are returned with a non-empty reuse_file rather than re-read from GPU.
// Returns an empty vector when the context is deferred or no textures are found.
std::vector<CaptureTexResult>
capture_pixel_textures(ID3D11DeviceContext* ctx,
                       std::uint32_t        draw_id,
                       std::uint32_t        frame_id);

// Dedup map management. Call dedup_begin_frame() at the start of each capture
// frame. Call dedup_tex_register() after successfully writing a new texture so
// subsequent draws that bind the same ID3D11Texture2D* can reuse the file.
void dedup_begin_frame();
void dedup_tex_register(ID3D11Texture2D*                    ptr,
                        std::string_view                    filename,
                        const openripper::TextureSnapshot&  snap_meta);

} // namespace openripper::backends::d3d11
