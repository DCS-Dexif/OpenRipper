// OpenRipper - src/runtime/injector.hpp
//
// Out-of-process helpers used by the CLI to launch a target executable and
// inject a backend DLL into it. Pure Win32; no graphics knowledge here.

#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace openripper::runtime {

struct LaunchOptions {
    // Absolute path to the target .exe to launch.
    std::filesystem::path                 executable;

    // Extra arguments forwarded to the target (each individually quoted).
    std::vector<std::wstring>             arguments;

    // Working directory for the new process. Defaults to the exe's parent.
    std::optional<std::filesystem::path>  working_dir;

    // OpenRipper backend DLL to inject (e.g. OpenRipper_d3d11.dll).
    std::filesystem::path                 inject_dll;

    // If true, the target is launched suspended and resumed without any DLL
    // injection. Useful for smoke-testing the launcher in isolation.
    bool                                  start_suspended_only = false;
};

struct LaunchResult {
    bool          ok            = false;
    std::uint32_t pid           = 0;
    std::string   error_message;
};

// Create the target process suspended, optionally inject the backend DLL via
// a remote LoadLibraryW thread, then resume the main thread. Returns ok=true
// on success.
LaunchResult launch_and_inject(const LaunchOptions& opts);

// Inject the given DLL into an already-running PID via remote LoadLibraryW.
LaunchResult inject_into_pid(std::uint32_t pid,
                             const std::filesystem::path& dll);

} // namespace openripper::runtime
