// OpenRipper - src/backends/d3d12/hooks_d3d12.hpp

#pragma once

#include <d3d12.h>

namespace openripper::backends::d3d12 {

// MinHook-based vtable hooks for Present, Draw*, IA*, PSO, resource creation.
bool install_hooks();
void remove_hooks();

// Write session.json if frames have been captured (called from DllMain
// DLL_PROCESS_DETACH when lpvReserved != nullptr).
void flush_session_manifest_now();

// Pre-arm capture if capture_frame == 0.
void activate_capture_if_frame_zero();

// Device and queue pointers captured from CreateCommittedResource and
// ExecuteCommandLists hooks; used by overlay and capture code.
extern ID3D12Device*       g_device;
extern ID3D12CommandQueue* g_game_queue;

} // namespace openripper::backends::d3d12
