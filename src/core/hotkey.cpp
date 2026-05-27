// OpenRipper - src/core/hotkey.cpp

#include "hotkey.hpp"
#include "logger.hpp"

#include <windows.h>
#include <atomic>

namespace openripper {
namespace {

std::atomic<bool>   g_stop{false};
HANDLE              g_thread{nullptr};
unsigned int        g_vk_code{0};

// Pointers to the calling backend's freeze state.
std::atomic<std::uint32_t>* g_freeze_remaining{nullptr};
const std::uint32_t*         g_freeze_count{nullptr};

DWORD WINAPI hotkey_thread_proc(LPVOID) {
    bool was_pressed = false;
    while (!g_stop.load(std::memory_order_relaxed)) {
        const bool pressed = (::GetAsyncKeyState(static_cast<int>(g_vk_code)) & 0x8000) != 0;

        if (pressed && !was_pressed) {
            // Rising edge: arm capture for the next frame.
            // Only arm if not already active (don't interrupt an ongoing burst).
            if (g_freeze_remaining->load(std::memory_order_relaxed) == 0) {
                g_freeze_remaining->store(*g_freeze_count, std::memory_order_release);
                OR_LOG_INFO("hotkey: 0x{:02X} pressed — queued {} frame(s) for capture",
                            g_vk_code, *g_freeze_count);
            }
        }

        was_pressed = pressed;
        ::Sleep(16);
    }
    return 0;
}

} // namespace

void start_hotkey_thread(unsigned int               vk_code,
                         std::atomic<std::uint32_t>& freeze_remaining,
                         const std::uint32_t&        freeze_count)
{
    if (vk_code == 0) {
        OR_LOG_INFO("hotkey: disabled (rip_hotkey=0)");
        return;
    }
    g_vk_code          = vk_code;
    g_freeze_remaining = &freeze_remaining;
    g_freeze_count     = &freeze_count;
    g_stop.store(false, std::memory_order_relaxed);
    g_thread = ::CreateThread(nullptr, 0, hotkey_thread_proc, nullptr, 0, nullptr);
    if (g_thread)
        OR_LOG_INFO("hotkey: polling thread started (VK=0x{:02X})", vk_code);
    else
        OR_LOG_WARN("hotkey: CreateThread failed (gle={})", ::GetLastError());
}

void stop_hotkey_thread() {
    if (!g_thread) return;
    g_stop.store(true, std::memory_order_release);
    ::WaitForSingleObject(g_thread, 500);
    ::CloseHandle(g_thread);
    g_thread           = nullptr;
    g_freeze_remaining = nullptr;
    g_freeze_count     = nullptr;
}

} // namespace openripper
