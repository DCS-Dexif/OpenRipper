// OpenRipper - src/backends/d3d11/hotkey_d3d11.cpp

#include "hotkey_d3d11.hpp"
#include "runtime_state.hpp"

#include "core/logger.hpp"

#include <windows.h>
#include <atomic>

namespace openripper::backends::d3d11 {
namespace {

std::atomic<bool> g_hotkey_stop{false};
HANDLE            g_hotkey_thread{nullptr};
unsigned int      g_vk_code{0};

DWORD WINAPI hotkey_thread_proc(LPVOID) {
    bool was_pressed = false;
    while (!g_hotkey_stop.load(std::memory_order_relaxed)) {
        const bool pressed = (::GetAsyncKeyState(static_cast<int>(g_vk_code)) & 0x8000) != 0;

        if (pressed && !was_pressed) {
            // Rising edge: arm capture for the next frame.
            // Only arm if not already active (don't interrupt an ongoing burst).
            if (g_freeze_frames_remaining.load(std::memory_order_relaxed) == 0) {
                g_freeze_frames_remaining.store(g_freeze_count, std::memory_order_release);
                OR_LOG_INFO("hotkey: F10 pressed — queued {} frame(s) for capture",
                            g_freeze_count);
            }
        }

        was_pressed = pressed;
        ::Sleep(16);
    }
    return 0;
}

} // namespace

void start_hotkey_thread(unsigned int vk_code) {
    if (vk_code == 0) {
        OR_LOG_INFO("hotkey: disabled (rip_hotkey=0)");
        return;
    }
    g_vk_code = vk_code;
    g_hotkey_stop.store(false, std::memory_order_relaxed);
    g_hotkey_thread = ::CreateThread(nullptr, 0, hotkey_thread_proc, nullptr, 0, nullptr);
    if (g_hotkey_thread)
        OR_LOG_INFO("hotkey: polling thread started (VK=0x{:02X})", vk_code);
    else
        OR_LOG_WARN("hotkey: CreateThread failed (gle={})", ::GetLastError());
}

void stop_hotkey_thread() {
    if (!g_hotkey_thread) return;
    g_hotkey_stop.store(true, std::memory_order_release);
    ::WaitForSingleObject(g_hotkey_thread, 500);
    ::CloseHandle(g_hotkey_thread);
    g_hotkey_thread = nullptr;
}

} // namespace openripper::backends::d3d11
