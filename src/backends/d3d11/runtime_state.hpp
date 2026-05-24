// OpenRipper - src/backends/d3d11/runtime_state.hpp
//
// Globals written once by dllmain's init_thread (after Config::load) and then
// read by hooks_d3d11 and capture_d3d11. All writes finish before
// install_hooks() is called, so all subsequent accesses are read-only and need
// no synchronisation — except g_capture_frame_target, which is atomic because
// Stage 4 will allow the hotkey thread to write it at any time.

#pragma once

#include <atomic>
#include <filesystem>
#include <limits>

namespace openripper::backends::d3d11 {

// Root directory under which .obj files (and later textures) are written.
// Set from Config::output_dir; the directory is created on init.
inline std::filesystem::path g_output_dir{"captures"};

// Absolute frame number (0-based, counting from the first Present after DLL
// load) on which to capture all draws. UINT64_MAX means disabled.
// Stage 4 will write this from the hotkey handler.
inline std::atomic<std::uint64_t> g_capture_frame_target{
    std::numeric_limits<std::uint64_t>::max()};

} // namespace openripper::backends::d3d11
