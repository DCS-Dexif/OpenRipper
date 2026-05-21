// OpenRipper.exe - out-of-process launcher.
//
// Responsibilities:
//   1. Parse a small set of command-line arguments.
//   2. Resolve which backend DLL to inject (defaults to
//      OpenRipper_<api>.dll next to this exe).
//   3. Launch the target suspended, inject the backend, and resume execution.
//
// The CLI deliberately does NOT do anything graphics-related itself; all
// hooking lives inside the backend DLLs once they are loaded into the
// target process.

#include "core/config.hpp"
#include "core/logger.hpp"
#include "runtime/injector.hpp"

#include <windows.h>

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;

namespace {

void print_usage() {
    std::cout <<
R"(OpenRipper - open-source 3D asset extraction toolkit

USAGE:
  OpenRipper.exe --target <game.exe> [--backend d3d11] [options]
  OpenRipper.exe --pid <pid> [--backend d3d11] [options]

OPTIONS:
  --target  <path>     Target executable to launch and rip.
  --backend <api>      Capture backend: d3d11 (default). d3d12/d3d9/opengl/
                       vulkan are reserved for future stages.
  --dll     <path>     Override backend DLL path. Default: OpenRipper_<backend>.dll
                       beside this executable.
  --pid     <pid>      Attach to an already-running PID instead of launching.
  --config  <path>     Load a key=value config file.
  --log     <path>     Override log file path (default: OpenRipper.log in cwd).
  --verbose            Set log level to debug.
  --trace              Set log level to trace.
  --no-inject          Launch suspended and resume without injecting (smoke test).
  --                   End of options; remaining args are forwarded to the target.
  -h, --help           Show this message.

NOTES:
  * OpenRipper is intended for personal archival, modding, and preservation use.
  * It is not designed to evade anti-cheat systems and will refuse to attach to
    titles protected by EAC/BattlEye/VAC/Vanguard (planned).
)";
}

std::string backend_dll_name(std::string_view backend) {
    if (backend == "d3d11")  return "OpenRipper_d3d11.dll";
    if (backend == "d3d12")  return "OpenRipper_d3d12.dll";
    if (backend == "d3d9")   return "OpenRipper_d3d9.dll";
    if (backend == "opengl") return "OpenRipper_opengl.dll";
    if (backend == "vulkan") return "OpenRipper_vulkan.dll";
    return "OpenRipper_d3d11.dll";
}

std::wstring widen_utf8(const std::string& s) {
    if (s.empty()) return {};
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(),
                                        static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                          w.data(), n);
    return w;
}

// Return the directory containing the running OpenRipper.exe so we can find
// sibling backend DLLs without depending on cwd.
fs::path own_directory() {
    wchar_t buf[MAX_PATH] = {};
    ::GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return fs::path(buf).parent_path();
}

} // namespace

int main(int argc, char** argv) {
    using namespace openripper;

    std::string                target_path;
    std::string                backend      = "d3d11";
    std::string                dll_override;
    std::string                cfg_path;
    std::string                log_override;
    std::vector<std::wstring>  forward_args;
    std::uint32_t              attach_pid   = 0;
    bool                       no_inject    = false;
    LogLevel                   verbosity    = LogLevel::Info;
    bool                       verbosity_set = false;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];

        auto next = [&]() -> std::string {
            if (i + 1 >= argc) {
                std::cerr << "missing value for " << a << "\n";
                std::exit(2);
            }
            return argv[++i];
        };

        if      (a == "-h" || a == "--help") { print_usage(); return 0; }
        else if (a == "--target")    target_path  = next();
        else if (a == "--backend")   backend      = next();
        else if (a == "--dll")       dll_override = next();
        else if (a == "--pid")       attach_pid   = static_cast<std::uint32_t>(std::stoul(next()));
        else if (a == "--config")    cfg_path     = next();
        else if (a == "--log")       log_override = next();
        else if (a == "--verbose")   { verbosity = LogLevel::Debug; verbosity_set = true; }
        else if (a == "--trace")     { verbosity = LogLevel::Trace; verbosity_set = true; }
        else if (a == "--no-inject") no_inject    = true;
        else if (a == "--") {
            for (++i; i < argc; ++i) forward_args.push_back(widen_utf8(argv[i]));
            break;
        }
        else {
            std::cerr << "unknown argument: " << a << "\n";
            print_usage();
            return 2;
        }
    }

    Config cfg = cfg_path.empty() ? Config{} : Config::load(cfg_path);
    if (verbosity_set)       cfg.log_level = verbosity;
    if (!log_override.empty()) cfg.log_file = log_override;

    Logger::init(cfg.log_file, cfg.log_level);
    OR_LOG_INFO("OpenRipper CLI starting (backend={}, no_inject={})", backend, no_inject);

    // Resolve which DLL to inject: explicit --dll wins, otherwise the
    // canonical OpenRipper_<api>.dll beside this exe.
    fs::path dll_path = dll_override.empty()
                            ? (own_directory() / backend_dll_name(backend))
                            : fs::path(dll_override);

    runtime::LaunchResult res;

    if (attach_pid != 0) {
        OR_LOG_INFO("attaching to PID {} with {}", attach_pid, dll_path.string());
        res = runtime::inject_into_pid(attach_pid, dll_path);
    } else {
        if (target_path.empty()) {
            std::cerr << "error: --target or --pid required\n\n";
            print_usage();
            return 2;
        }
        runtime::LaunchOptions opts;
        opts.executable           = target_path;
        opts.arguments            = forward_args;
        opts.inject_dll           = dll_path;
        opts.start_suspended_only = no_inject;
        res = runtime::launch_and_inject(opts);
    }

    if (!res.ok) {
        OR_LOG_ERROR("launch/injection failed: {}", res.error_message);
        std::cerr << "OpenRipper: " << res.error_message << "\n";
        Logger::shutdown();
        return 1;
    }

    OR_LOG_INFO("running (PID {}). CLI exiting; backend now lives inside target.", res.pid);
    Logger::shutdown();
    return 0;
}
