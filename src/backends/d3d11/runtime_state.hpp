// OpenRipper - src/backends/d3d11/runtime_state.hpp
//
// Globals written once by dllmain's init_thread (after Config::load) and then
// read by hooks_d3d11 and capture_d3d11. All writes finish before
// install_hooks() is called, so all subsequent accesses are read-only and need
// no synchronisation — except g_capture_frame_target, which is atomic because
// Stage 4 will allow the hotkey thread to write it at any time.

#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <string>

namespace openripper::backends::d3d11 {

// Root directory for this session (includes timestamp subdir).
// Set from Config::output_dir + session timestamp in dllmain init_thread.
inline std::filesystem::path g_output_dir{"captures"};

// Absolute frame number (0-based) on which to trigger capture. UINT64_MAX = disabled.
inline std::atomic<std::uint64_t> g_capture_frame_target{
    std::numeric_limits<std::uint64_t>::max()};

// Number of frames remaining in the current freeze burst (0 = idle).
// Written by the hotkey thread and the config-trigger arm path.
// Read and decremented by hooked_present on the render thread.
inline std::atomic<std::uint32_t> g_freeze_frames_remaining{0};

// Frames to capture per trigger (from Config::freeze_frames, default 1).
// Set once by init_thread before hooks are installed; read-only after that.
inline std::uint32_t g_freeze_count{1};

// Session identifier (timestamp string, e.g. "20260525_232408").
// Written once by init_thread; used in session manifest.
inline std::string g_session_id;

// Base name of the target executable (e.g. "d3d11_cube.exe").
// Written once by init_thread; used in session manifest.
inline std::string g_target_exe_name;

} // namespace openripper::backends::d3d11
