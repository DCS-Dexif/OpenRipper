// OpenRipper - src/core/config.cpp

#include "config.hpp"

#include <cctype>
#include <fstream>
#include <string>

namespace openripper {
namespace {

std::string trim(std::string s) {
    auto is_space = [](unsigned char c) { return std::isspace(c) != 0; };
    while (!s.empty() && is_space(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
    while (!s.empty() && is_space(static_cast<unsigned char>(s.back())))  s.pop_back();
    return s;
}

LogLevel parse_level(const std::string& v) {
    if (v == "trace") return LogLevel::Trace;
    if (v == "debug") return LogLevel::Debug;
    if (v == "info")  return LogLevel::Info;
    if (v == "warn")  return LogLevel::Warn;
    if (v == "error") return LogLevel::Error;
    if (v == "off")   return LogLevel::Off;
    return LogLevel::Info;
}

bool parse_bool(const std::string& v) {
    return (v == "1" || v == "true" || v == "yes" || v == "on");
}

} // namespace

Config Config::load(const std::filesystem::path& path) {
    Config cfg;
    std::ifstream f(path);
    if (!f) return cfg;

    std::string line;
    while (std::getline(f, line)) {
        // Strip trailing comments and surrounding whitespace.
        if (const auto h = line.find('#'); h != std::string::npos) {
            line.erase(h);
        }
        line = trim(std::move(line));
        if (line.empty()) continue;

        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;

        const std::string key = trim(line.substr(0, eq));
        const std::string val = trim(line.substr(eq + 1));

        if      (key == "log_file")           cfg.log_file           = val;
        else if (key == "output_dir")         cfg.output_dir         = val;
        else if (key == "log_level")          cfg.log_level          = parse_level(val);
        else if (key == "rip_hotkey")         cfg.rip_hotkey         = static_cast<unsigned int>(std::stoul(val, nullptr, 0));
        else if (key == "time_freeze_on_rip") cfg.time_freeze_on_rip = parse_bool(val);
        else if (key == "capture_frame")      cfg.capture_frame      = std::stoull(val);
        else {
            OR_LOG_TRACE("config: unknown key '{}' (ignored)", key);
        }
    }
    return cfg;
}

} // namespace openripper
