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
#include "core/hotkey.hpp"
#include "core/logger.hpp"

#include <windows.h>

#include <chrono>
#include <ctime>
#include <filesystem>
#include <format>
#include <limits>

namespace {

DWORD WINAPI init_thread(LPVOID) {
    // Locate the host EXE directory so relative paths in OpenRipper.cfg and
    // the output directory are anchored to a predictable location.
    wchar_t exe_buf[MAX_PATH] = {};
    ::GetModuleFileNameW(nullptr, exe_buf, MAX_PATH);
    const std::filesystem::path exe_path(exe_buf);
    const std::filesystem::path exe_dir = exe_path.parent_path();

    // ---- Logger init --------------------------------------------------------
    openripper::Logger::init(exe_dir / "OpenRipper.log", openripper::LogLevel::Info);
    OR_LOG_INFO("OpenRipper_d3d11.dll loaded into: {}", exe_path.filename().string());

    // Store target exe name for session manifest.
    openripper::backends::d3d11::g_target_exe_name = exe_path.filename().string();

    // ---- Config loading -----------------------------------------------------
    const std::filesystem::path cfg_path = exe_dir / "OpenRipper.cfg";
    openripper::Config cfg;
    if (std::filesystem::exists(cfg_path)) {
        cfg = openripper::Config::load(cfg_path);
        OR_LOG_INFO("config loaded from {}", cfg_path.filename().string());
    } else {
        OR_LOG_INFO("no OpenRipper.cfg found - using defaults (capture disabled)");
    }

    openripper::Logger::set_level(cfg.log_level);

    // ---- Runtime state ------------------------------------------------------
    openripper::backends::d3d11::g_capture_frame_target.store(
        cfg.capture_frame, std::memory_order_relaxed);
    openripper::backends::d3d11::g_freeze_count         = (cfg.freeze_frames > 0) ? cfg.freeze_frames : 1;
    openripper::backends::d3d11::g_time_freeze_on_rip   = cfg.time_freeze_on_rip;
    openripper::backends::d3d11::g_flip_winding         = cfg.flip_winding;

    // ---- Session output directory (always created — hotkey can trigger at any time) ---
    const auto out = cfg.output_dir.is_absolute()
                     ? cfg.output_dir
                     : exe_dir / cfg.output_dir;

    // Append a timestamp subdirectory: captures/YYYYMMDD_HHMMSS/
    {
        auto now_t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        std::tm tm_local{};
        ::localtime_s(&tm_local, &now_t);
        openripper::backends::d3d11::g_session_id = std::format(
            "{:04}{:02}{:02}_{:02}{:02}{:02}",
            tm_local.tm_year + 1900, tm_local.tm_mon + 1, tm_local.tm_mday,
            tm_local.tm_hour, tm_local.tm_min, tm_local.tm_sec);
    }
    openripper::backends::d3d11::g_output_dir =
        out / openripper::backends::d3d11::g_session_id;

    {
        std::error_code ec;
        std::filesystem::create_directories(openripper::backends::d3d11::g_output_dir, ec);
        if (ec)
            OR_LOG_WARN("could not create output dir '{}': {}",
                        openripper::backends::d3d11::g_output_dir.string(), ec.message());
        else
            OR_LOG_INFO("capture output dir: {}",
                        openripper::backends::d3d11::g_output_dir.string());
    }

    if (cfg.capture_frame != std::numeric_limits<std::uint64_t>::max())
        OR_LOG_INFO("capture scheduled for frame {} ({} frame(s))",
                    cfg.capture_frame, openripper::backends::d3d11::g_freeze_count);

    // ---- Hook installation --------------------------------------------------
    if (!openripper::backends::d3d11::install_hooks()) {
        OR_LOG_ERROR("D3D11 hook installation failed - backend is inert.");
        return 1;
    }

    openripper::backends::d3d11::activate_capture_if_frame_zero();

    // ---- Hotkey thread ------------------------------------------------------
    openripper::start_hotkey_thread(cfg.rip_hotkey,
        openripper::backends::d3d11::g_freeze_frames_remaining,
        openripper::backends::d3d11::g_freeze_count);

    return 0;
}

} // namespace

BOOL APIENTRY DllMain(HMODULE mod, DWORD reason, LPVOID reserved) {
    switch (reason) {
        case DLL_PROCESS_ATTACH: {
            ::DisableThreadLibraryCalls(mod);
            HANDLE th = ::CreateThread(nullptr, 0, init_thread, nullptr, 0, nullptr);
            if (th) ::CloseHandle(th);
            break;
        }
        case DLL_PROCESS_DETACH: {
            if (reserved != nullptr) {
                // Process termination: all other threads already dead.
                // COM teardown is unsafe (game released its D3D device).
                // Write session manifest only; OS reclaims everything else.
                openripper::backends::d3d11::flush_session_manifest_now();
            } else {
                // FreeLibrary: safe to do full cleanup.
                openripper::backends::d3d11::remove_hooks();
            }
            openripper::Logger::shutdown();
            break;
        }
        default:
            break;
    }
    return TRUE;
}
