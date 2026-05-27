// OpenRipper - src/backends/d3d12/runtime_state.hpp
//
// Globals written once by dllmain's init_thread, then read by hooks and
// capture code. Atomics are used for state shared with the hotkey thread.

#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <string>

namespace openripper::backends::d3d12 {

inline std::filesystem::path g_output_dir{"captures"};

// Target frame for automatic capture (UINT64_MAX = disabled).
inline std::atomic<std::uint64_t> g_capture_frame_target{
    std::numeric_limits<std::uint64_t>::max()};

// Frames remaining in current burst (0 = idle). Written by hotkey thread and
// config-trigger arm path; decremented by hooked_present on the render thread.
inline std::atomic<std::uint32_t> g_freeze_frames_remaining{0};

// Frames per trigger (Config::freeze_frames, default 1). Read-only after init.
inline std::uint32_t g_freeze_count{1};

// Stage 4.1: skip real Present during active capture frames.
inline bool g_time_freeze_on_rip{false};

// Stage 4.2: reverse OBJ face winding on export.
inline bool g_flip_winding{false};

// Skip re-reading duplicate texture resources within a frame.
inline bool g_dedup{false};

inline std::string g_session_id;
inline std::string g_target_exe_name;

} // namespace openripper::backends::d3d12
