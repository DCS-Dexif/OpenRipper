// OpenRipper - src/backends/d3d11/hooks_d3d11.hpp

#pragma once

namespace openripper::backends::d3d11 {

// Install vtable hooks for D3D11 + DXGI. Idempotent: only the first
// successful call performs the work. Returns true on success, false if
// MinHook initialization, vtable acquisition or hook activation failed.
bool install_hooks();

// Remove every hook installed by install_hooks. No-op if hooks were never
// installed.
void remove_hooks();

} // namespace openripper::backends::d3d11
