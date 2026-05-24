// OpenRipper - src/backends/d3d11/dllmain.cpp
//
// Entry point for OpenRipper_d3d11.dll.
//
// When the injector LoadLibraryW's us into the target, DllMain fires under
// the loader lock. Doing real work there (file I/O, COM calls, D3D init,
// blocking) is undefined behavior in the general case. We instead spawn a
// dedicated worker thread that performs logger setup, config loading, and
// hook installation once the loader has released its lock.

#include "hooks_d3d11.hpp"
#include "runtime_state.hpp"

#include "core/config.hpp"
#include "core/logger.hpp"

#include <windows.h>

#include <filesystem>
#include <limits>

namespace {

DWORD WINAPI init_thread(LPVOID) {
    // Locate the host EXE directory so relative paths in OpenRipper.cfg and
    // the output directory are anchored to a predictable location.
    wchar_t exe_buf[MAX_PATH] = {};
    ::GetModuleFileNameW(nullptr, exe_buf, MAX_PATH);
    const std::filesystem::path exe_dir = std::filesystem::path(exe_buf).parent_path();

    // ---- Logger init (best-effort; falls back to OutputDebugString+stderr) -
    openripper::Logger::init(exe_dir / "OpenRipper.log", openripper::LogLevel::Info);
    OR_LOG_INFO("OpenRipper_d3d11.dll loaded into: {}",
                std::filesystem::path(exe_buf).filename().string());

    // ---- Config loading (optional; default Config used if .cfg absent) -----
    const std::filesystem::path cfg_path = exe_dir / "OpenRipper.cfg";
    openripper::Config cfg;
    if (std::filesystem::exists(cfg_path)) {
        cfg = openripper::Config::load(cfg_path);
        OR_LOG_INFO("config loaded from {}", cfg_path.filename().string());
    } else {
        OR_LOG_INFO("no OpenRipper.cfg found - using defaults (capture disabled)");
    }

    // Apply log level from config (overrides the Info default set above).
    openripper::Logger::set_level(cfg.log_level);

    // ---- Runtime state (read by hooks_d3d11.cpp and capture_d3d11.cpp) -----
    openripper::backends::d3d11::g_capture_frame_target.store(
        cfg.capture_frame, std::memory_order_relaxed);

    // Resolve and create the output directory next to the EXE.
    // If output_dir is absolute we use it as-is; relative paths are anchored
    // to the EXE directory so captures are always findable.
    const auto out = cfg.output_dir.is_absolute()
                     ? cfg.output_dir
                     : exe_dir / cfg.output_dir;
    openripper::backends::d3d11::g_output_dir = out;

    if (cfg.capture_frame != std::numeric_limits<std::uint64_t>::max()) {
        // Only create the output dir if capture is actually enabled.
        std::error_code ec;
        std::filesystem::create_directories(out, ec);
        if (ec)
            OR_LOG_WARN("could not create output dir '{}': {}", out.string(), ec.message());
        else
            OR_LOG_INFO("capture output dir: {}", out.string());

        OR_LOG_INFO("capture scheduled for frame {}", cfg.capture_frame);
    }

    // ---- Hook installation --------------------------------------------------
    if (!openripper::backends::d3d11::install_hooks()) {
        OR_LOG_ERROR("D3D11 hook installation failed - backend is inert.");
        return 1;
    }

    // Handle the special case where capture_frame == 0 (before the first
    // Present fires, so we can't pre-arm from inside hooked_present).
    openripper::backends::d3d11::activate_capture_if_frame_zero();
    return 0;
}

} // namespace

BOOL APIENTRY DllMain(HMODULE mod, DWORD reason, LPVOID /*reserved*/) {
    switch (reason) {
        case DLL_PROCESS_ATTACH: {
            // Skip DLL_THREAD_ATTACH/DETACH notifications (no per-thread state).
            ::DisableThreadLibraryCalls(mod);

            HANDLE th = ::CreateThread(nullptr, 0, init_thread, nullptr, 0, nullptr);
            if (th) ::CloseHandle(th);
            break;
        }
        case DLL_PROCESS_DETACH: {
            openripper::backends::d3d11::remove_hooks();
            openripper::Logger::shutdown();
            break;
        }
        default:
            break;
    }
    return TRUE;
}
