// OpenRipper - src/backends/d3d11/overlay_d3d11.hpp
//
// D2D1 text overlay rendered onto the swap chain back buffer before Present.
// Falls back to a window-title flash if D2D1 cannot attach (MSAA / HDR swap
// chains, or DWrite init failure).
//
// All public functions must be called from the render thread (inside
// hooked_present) — no internal locking.

#pragma once

#include <cstdint>
#include <dxgi.h>

namespace openripper::backends::d3d11 {

// Call before Present (from hooked_present). Lazy-inits D2D1 on first call.
// Draws the overlay text if g_overlay_frames > 0, then decrements.
void overlay_draw(IDXGISwapChain* sc, std::uint64_t frame_id);

// Arms the overlay to display the "CAPTURED" text for `frames_to_show` frames.
// Also handles the window-title fallback path.
void overlay_notify(std::uint64_t frame_id, std::uint32_t draw_count,
                    std::uint32_t frames_to_show = 120);

// Releases all D2D1/DWrite COM objects. Restores window title if fallback
// is active. Call from remove_hooks().
void overlay_shutdown();

} // namespace openripper::backends::d3d11
