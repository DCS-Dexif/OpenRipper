// OpenRipper - src/runtime/injector.cpp
//
// Implementation notes:
//
// The injector uses the canonical CreateRemoteThread + LoadLibraryW pattern.
// It's old, simple, and well understood. It works in two phases:
//
//   1. Spawn the target in CREATE_SUSPENDED state via CreateProcessW.
//   2. VirtualAllocEx a small buffer in the target, write the wide path of
//      our backend DLL, and CreateRemoteThread at kernel32!LoadLibraryW with
//      that buffer as the parameter. Wait for the thread, then resume the
//      target's main thread.
//
// Rationale for keeping it this simple:
//   - Reliable on every supported Windows version (the kernel32 base address
//     is identical across all processes in a given Windows session, so the
//     LoadLibraryW pointer obtained in our process matches the target's).
//   - No reliance on shellcode or hand-crafted thread stubs.
//   - Trivial to swap out later for manual mapping, APC injection, or a
//     debugger-attach path if a particular title resists this approach.

#include "injector.hpp"

#include "core/logger.hpp"

#include <windows.h>

#include <sstream>
#include <vector>

namespace openripper::runtime {
namespace {

std::string last_error_string(DWORD code = ::GetLastError()) {
    LPSTR buf = nullptr;
    const DWORD len = ::FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<LPSTR>(&buf), 0, nullptr);
    std::string s = len ? std::string(buf, len) : std::string("<unknown>");
    if (buf) ::LocalFree(buf);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
    return s + " (code " + std::to_string(code) + ")";
}

std::wstring build_command_line(const std::filesystem::path& exe,
                                const std::vector<std::wstring>& args) {
    std::wstringstream ss;
    ss << L'"' << exe.wstring() << L'"';
    for (const auto& a : args) {
        ss << L' ' << L'"' << a << L'"';
    }
    return ss.str();
}

// Remote-inject a single DLL into `process` via LoadLibraryW. Returns false
// and populates `err_out` on any step that fails.
bool inject_loadlibrary_remote(HANDLE process,
                               const std::filesystem::path& dll_path,
                               std::string& err_out) {
    const std::wstring wpath = std::filesystem::absolute(dll_path).wstring();
    const SIZE_T       bytes = (wpath.size() + 1) * sizeof(wchar_t);

    void* remote_buf = ::VirtualAllocEx(process, nullptr, bytes,
                                        MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote_buf) {
        err_out = "VirtualAllocEx failed: " + last_error_string();
        return false;
    }

    SIZE_T written = 0;
    if (!::WriteProcessMemory(process, remote_buf, wpath.c_str(), bytes, &written) ||
        written != bytes) {
        err_out = "WriteProcessMemory failed: " + last_error_string();
        ::VirtualFreeEx(process, remote_buf, 0, MEM_RELEASE);
        return false;
    }

    HMODULE k32 = ::GetModuleHandleW(L"kernel32.dll");
    auto load_library_w = reinterpret_cast<LPTHREAD_START_ROUTINE>(
        ::GetProcAddress(k32, "LoadLibraryW"));
    if (!load_library_w) {
        err_out = "GetProcAddress(LoadLibraryW) failed: " + last_error_string();
        ::VirtualFreeEx(process, remote_buf, 0, MEM_RELEASE);
        return false;
    }

    HANDLE th = ::CreateRemoteThread(process, nullptr, 0,
                                     load_library_w, remote_buf, 0, nullptr);
    if (!th) {
        err_out = "CreateRemoteThread failed: " + last_error_string();
        ::VirtualFreeEx(process, remote_buf, 0, MEM_RELEASE);
        return false;
    }

    ::WaitForSingleObject(th, INFINITE);

    // LoadLibraryW returns the loaded HMODULE in the thread's exit code.
    // A zero exit indicates the DLL failed to load (bad path, dependency
    // missing, DllMain returned FALSE, etc.).
    DWORD load_addr = 0;
    ::GetExitCodeThread(th, &load_addr);
    ::CloseHandle(th);
    ::VirtualFreeEx(process, remote_buf, 0, MEM_RELEASE);

    if (load_addr == 0) {
        err_out = "remote LoadLibraryW returned NULL (DLL failed to load inside target)";
        return false;
    }
    return true;
}

} // namespace

LaunchResult launch_and_inject(const LaunchOptions& opts) {
    LaunchResult res;

    if (!std::filesystem::exists(opts.executable)) {
        res.error_message = "target executable not found: " + opts.executable.string();
        return res;
    }
    if (!opts.start_suspended_only && !std::filesystem::exists(opts.inject_dll)) {
        res.error_message = "backend DLL not found: " + opts.inject_dll.string();
        return res;
    }

    STARTUPINFOW         si{};
    PROCESS_INFORMATION  pi{};
    si.cb = sizeof(si);

    const std::wstring cmdline = build_command_line(opts.executable, opts.arguments);
    const std::wstring wdir    = (opts.working_dir
                                      ? *opts.working_dir
                                      : opts.executable.parent_path()).wstring();

    // CreateProcessW requires a writable command-line buffer.
    std::vector<wchar_t> cmd_buf(cmdline.begin(), cmdline.end());
    cmd_buf.push_back(L'\0');

    const BOOL ok = ::CreateProcessW(
        opts.executable.wstring().c_str(),
        cmd_buf.data(),
        nullptr, nullptr,
        FALSE,
        CREATE_SUSPENDED,
        nullptr,
        wdir.empty() ? nullptr : wdir.c_str(),
        &si, &pi);

    if (!ok) {
        res.error_message = "CreateProcessW failed: " + last_error_string();
        return res;
    }

    res.pid = pi.dwProcessId;
    OR_LOG_INFO("spawned PID {} (suspended)", res.pid);

    if (!opts.start_suspended_only) {
        std::string err;
        if (!inject_loadlibrary_remote(pi.hProcess, opts.inject_dll, err)) {
            res.error_message = err;
            ::TerminateProcess(pi.hProcess, 1);
            ::CloseHandle(pi.hThread);
            ::CloseHandle(pi.hProcess);
            return res;
        }
        OR_LOG_INFO("injected {} into PID {}", opts.inject_dll.string(), res.pid);
    }

    ::ResumeThread(pi.hThread);
    ::CloseHandle(pi.hThread);
    ::CloseHandle(pi.hProcess);

    res.ok = true;
    return res;
}

LaunchResult inject_into_pid(std::uint32_t pid, const std::filesystem::path& dll) {
    LaunchResult res;

    HANDLE proc = ::OpenProcess(
        PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
        PROCESS_VM_OPERATION  | PROCESS_VM_WRITE | PROCESS_VM_READ,
        FALSE, pid);
    if (!proc) {
        res.error_message = "OpenProcess failed: " + last_error_string();
        return res;
    }

    std::string err;
    if (!inject_loadlibrary_remote(proc, dll, err)) {
        res.error_message = err;
    } else {
        res.ok  = true;
        res.pid = pid;
        OR_LOG_INFO("injected {} into existing PID {}", dll.string(), pid);
    }
    ::CloseHandle(proc);
    return res;
}

} // namespace openripper::runtime
