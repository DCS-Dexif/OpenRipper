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

// Write the session manifest (session.json) and clear the frame accumulator.
// Safe to call during DLL_PROCESS_DETACH process-termination (lpvReserved !=
// null), where COM teardown is unsafe — COM cleanup is deliberately skipped.
void flush_session_manifest_now();

// If the configured capture_frame target is 0, pre-arm the capture active
// flag so the very first frame's draws are captured. Call this from
// dllmain's init_thread AFTER writing g_capture_frame_target in
// runtime_state.hpp and AFTER install_hooks() succeeds.
void activate_capture_if_frame_zero();

} // namespace openripper::backends::d3d11
