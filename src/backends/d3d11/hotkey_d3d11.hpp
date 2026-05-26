// OpenRipper - src/backends/d3d11/hotkey_d3d11.hpp
//
// Dedicated polling thread for the rip hotkey (default F10).
// Polls GetAsyncKeyState every 16 ms; on a rising edge, stores
// g_freeze_count into g_freeze_frames_remaining so hooked_present
// picks it up and arms capture for the next frame.

#pragma once

namespace openripper::backends::d3d11 {

// Spawns the hotkey polling thread.
// vk_code: VK_* virtual-key code (0 disables the thread entirely).
// Must be called after install_hooks() from init_thread.
void start_hotkey_thread(unsigned int vk_code);

// Signals the thread to stop and waits for it to exit.
// Called from remove_hooks() before MH_Uninitialize.
void stop_hotkey_thread();

} // namespace openripper::backends::d3d11
