// OpenRipper - src/backends/d3d12/overlay_d3d12.hpp
//
// D2D1 text overlay for D3D12, rendered via D3D11On12 interop.
// Falls back to window-title flash if D3D11On12 / D2D1 init fails.
//
// All public functions must be called from the render thread (hooked_present).

#pragma once

#include <cstdint>
#include <dxgi.h>

namespace openripper::backends::d3d12 {

// Draw the overlay for the current frame. Lazy-initialises D3D11On12 on first
// call using the device/queue captured from other hooks.
void overlay_draw(IDXGISwapChain* sc, std::uint64_t frame_id);

// Arm the overlay to show the "CAPTURED" message for ~2 s.
void overlay_notify(std::uint64_t frame_id, std::uint32_t draw_count,
                    std::uint32_t frames_to_show = 120);

// Release all D3D11On12/D2D1/DWrite COM objects.
void overlay_shutdown();

} // namespace openripper::backends::d3d12
