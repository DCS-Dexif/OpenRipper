// OpenRipper - src/backends/d3d11/dllmain.cpp
//
// Entry point for OpenRipper_d3d11.dll.
//
// When the injector LoadLibraryW's us into the target, DllMain fires under
// the loader lock. Doing real work there (file I/O, COM calls, D3D init,
// blocking) is undefined behavior in the general case. We instead spawn a
// dedicated worker thread that performs logger setup and hook installation
// once the loader has released its lock.

#include "hooks_d3d11.hpp"

#include "core/logger.hpp"

#include <windows.h>

#include <filesystem>

namespace {

DWORD WINAPI init_thread(LPVOID) {
    // Place the log file next to the host EXE so users find it without
    // hunting through working directories. If that fails for any reason the
    // logger silently falls back to OutputDebugString + stderr.
    wchar_t exe_buf[MAX_PATH] = {};
    ::GetModuleFileNameW(nullptr, exe_buf, MAX_PATH);

    const std::filesystem::path exe_path = exe_buf;
    const std::filesystem::path log_path = exe_path.parent_path() / "OpenRipper.log";

    openripper::Logger::init(log_path, openripper::LogLevel::Info);
    OR_LOG_INFO("OpenRipper_d3d11.dll loaded into host: {}",
                exe_path.filename().string());

    if (!openripper::backends::d3d11::install_hooks()) {
        OR_LOG_ERROR("D3D11 hook installation failed - backend is inert.");
    }
    return 0;
}

} // namespace

BOOL APIENTRY DllMain(HMODULE mod, DWORD reason, LPVOID /*reserved*/) {
    switch (reason) {
        case DLL_PROCESS_ATTACH: {
            // We have no thread-local state to maintain per-thread; skip the
            // DLL_THREAD_ATTACH/DETACH callbacks for a small perf win.
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
