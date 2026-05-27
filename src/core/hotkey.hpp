// OpenRipper - src/core/hotkey.hpp
//
// Shared hotkey polling thread — used by all graphics API backends.
// Polls GetAsyncKeyState every 16 ms; on a rising edge, stores
// freeze_count into freeze_remaining so hooked_present picks it up
// and arms capture for the next frame.

#pragma once

#include <atomic>
#include <cstdint>

namespace openripper {

// Spawns the hotkey polling thread.
//   vk_code          : VK_* virtual-key code (0 disables the thread entirely).
//   freeze_remaining : backend's g_freeze_frames_remaining atomic — written on key press.
//   freeze_count     : backend's g_freeze_count — value stored when arming.
// Must be called after install_hooks() from the backend's init_thread.
void start_hotkey_thread(unsigned int               vk_code,
                         std::atomic<std::uint32_t>& freeze_remaining,
                         const std::uint32_t&        freeze_count);

// Signals the thread to stop and waits for it to exit (up to 500 ms).
// Must be called before MH_Uninitialize / backend globals are destroyed.
void stop_hotkey_thread();

} // namespace openripper
