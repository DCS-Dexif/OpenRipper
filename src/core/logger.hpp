// OpenRipper - src/core/logger.hpp
//
// Minimal, dependency-free logger.
//
// Why not spdlog/fmt? Stage 1 ships with zero third-party requirements beyond
// MinHook so the bootstrap experience is "clone + cmake + build". std::format
// (C++20) covers the formatting needs without dragging in another library.
// The Logger surface intentionally mirrors a subset of the spdlog API so we
// can swap implementations later without touching call sites.

#pragma once

#include <cstdint>
#include <filesystem>
#include <format>
#include <string_view>

namespace openripper {

enum class LogLevel : std::uint8_t {
    Trace,
    Debug,
    Info,
    Warn,
    Error,
    Off,
};

class Logger {
public:
    // Open / re-open the log file. Pass an empty path to skip the file sink;
    // OutputDebugStringA and stderr remain active in that case.
    static void init(const std::filesystem::path& path,
                     LogLevel level = LogLevel::Info);

    // Flush and close the file sink. Safe to call multiple times.
    static void shutdown();

    static void set_level(LogLevel level) noexcept;
    static LogLevel level() noexcept;

    // Emit a pre-formatted message. Prefer the OR_LOG_* macros below.
    static void log(LogLevel level, std::string_view msg);
};

} // namespace openripper

// Convenience macros. Each expands to a std::format call so callers can write
// `OR_LOG_INFO("loaded {} bytes", size)` without referencing std::format.
#define OR_LOG_TRACE(...) ::openripper::Logger::log(::openripper::LogLevel::Trace, std::format(__VA_ARGS__))
#define OR_LOG_DEBUG(...) ::openripper::Logger::log(::openripper::LogLevel::Debug, std::format(__VA_ARGS__))
#define OR_LOG_INFO(...)  ::openripper::Logger::log(::openripper::LogLevel::Info,  std::format(__VA_ARGS__))
#define OR_LOG_WARN(...)  ::openripper::Logger::log(::openripper::LogLevel::Warn,  std::format(__VA_ARGS__))
#define OR_LOG_ERROR(...) ::openripper::Logger::log(::openripper::LogLevel::Error, std::format(__VA_ARGS__))
