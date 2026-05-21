// OpenRipper - src/core/logger.cpp

#include "logger.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <mutex>
#include <string>

#ifdef _WIN32
  #include <windows.h>
#endif

namespace openripper {
namespace {

// All sinks share a single mutex. The logger is meant to be called from hook
// callbacks running on the render thread; lock contention should be light
// because formatting is done outside the critical section.
std::mutex            g_mu;
std::ofstream         g_file;
std::atomic<LogLevel> g_level{LogLevel::Info};

const char* level_tag(LogLevel l) noexcept {
    switch (l) {
        case LogLevel::Trace: return "TRACE";
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info:  return "INFO ";
        case LogLevel::Warn:  return "WARN ";
        case LogLevel::Error: return "ERROR";
        default:              return "?????";
    }
}

// Wall-clock timestamp with millisecond resolution, ISO-ish format.
std::string timestamp() {
    using namespace std::chrono;
    const auto now = system_clock::now();
    const auto t   = system_clock::to_time_t(now);
    const auto ms  = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;

    std::tm tm{};
#ifdef _WIN32
    ::localtime_s(&tm, &t);
#else
    ::localtime_r(&t, &tm);
#endif

    char buf[32];
    std::snprintf(buf, sizeof(buf),
        "%04d-%02d-%02d %02d:%02d:%02d.%03d",
        tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
        tm.tm_hour, tm.tm_min, tm.tm_sec,
        static_cast<int>(ms.count()));
    return std::string(buf);
}

} // namespace

void Logger::init(const std::filesystem::path& path, LogLevel level) {
    std::lock_guard lk(g_mu);
    g_level.store(level, std::memory_order_relaxed);
    if (g_file.is_open()) {
        g_file.close();
    }
    if (!path.empty()) {
        // Append rather than truncate so multiple sessions accumulate.
        g_file.open(path, std::ios::out | std::ios::app);
    }
}

void Logger::shutdown() {
    std::lock_guard lk(g_mu);
    if (g_file.is_open()) {
        g_file.flush();
        g_file.close();
    }
}

void Logger::set_level(LogLevel level) noexcept {
    g_level.store(level, std::memory_order_relaxed);
}

LogLevel Logger::level() noexcept {
    return g_level.load(std::memory_order_relaxed);
}

void Logger::log(LogLevel level, std::string_view msg) {
    if (level == LogLevel::Off) return;
    if (level < g_level.load(std::memory_order_relaxed)) return;

    // Build the line outside the critical section.
    std::string line;
    line.reserve(msg.size() + 64);
    line.append(timestamp());
    line.append(" [");
    line.append(level_tag(level));
    line.append("] ");
    line.append(msg);
    line.push_back('\n');

    std::lock_guard lk(g_mu);
    if (g_file.is_open()) {
        g_file.write(line.data(), static_cast<std::streamsize>(line.size()));
        g_file.flush();
    }
    std::fwrite(line.data(), 1, line.size(), stderr);
#ifdef _WIN32
    ::OutputDebugStringA(line.c_str());
#endif
}

} // namespace openripper
