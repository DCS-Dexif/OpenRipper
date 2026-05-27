// OpenRipper - src/core/config.hpp
//
// Lightweight key=value config used by both the CLI and the injected backend
// DLLs. The format is deliberately trivial (no TOML/JSON dep) so the backend
// can parse it inside DllMain's init thread with zero allocations beyond the
// std lib.

#pragma once

#include <cstdint>
#include <filesystem>
#include <limits>

#include "logger.hpp"

namespace openripper {

struct Config {
    // Path of the rolling log file. Empty disables the file sink.
    std::filesystem::path log_file = "OpenRipper.log";

    // Root directory under which RipSessions are written.
    std::filesystem::path output_dir = "captures";

    // Verbosity floor for the logger.
    LogLevel log_level = LogLevel::Info;

    // VK_* virtual-key code for "rip current frame". 0 disables.
    // Default: 0x79 == VK_F10.
    unsigned int rip_hotkey = 0x79;

    // If true, the backend latches the frame and pauses presentation while
    // capture work runs. If false, the rip happens asynchronously.
    bool time_freeze_on_rip = false;

    // If true, OBJ face winding order is reversed (a,b,c → a,c,b).
    // Use for engines that expect CW front-face convention (imported meshes
    // appear inside-out in Blender without this).
    bool flip_winding = false;

    // If true, skip GPU readback and file writes for textures/meshes whose
    // API object pointer was already captured earlier in the same frame.
    // Reuses the first-written filename in the material manifest.
    bool dedup = false;

    // Absolute frame number (0-based, counting from the first Present after DLL
    // load) on which to capture all draws and write OBJ files. UINT64_MAX
    // means disabled.
    std::uint64_t capture_frame = std::numeric_limits<std::uint64_t>::max();

    // Number of consecutive frames to capture per trigger (hotkey or
    // capture_frame). 1 = single-frame rip; N > 1 = time-freeze burst.
    std::uint32_t freeze_frames = 1;

    // Parse a simple `key=value` file. `#` starts a comment. Missing keys
    // retain their defaults; unknown keys are silently ignored (logged at
    // Trace level for debugging). Returns the populated Config.
    static Config load(const std::filesystem::path& path);
};

} // namespace openripper
