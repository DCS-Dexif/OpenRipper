// OpenRipper - src/exporters/session_exporter.hpp
//
// Writes a per-session manifest (session.json) listing every captured frame.
// Called once at DLL detach from remove_hooks().

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace openripper::exporters {

struct FrameRecord {
    std::uint32_t frame_id     = 0;
    std::uint32_t draw_count   = 0;
    std::string   manifest_file; // basename only, e.g. "frame000120_materials.json"
};

// Writes captures/YYYYMMDD_HHMMSS/session.json.
// Returns false on I/O failure (logs the reason).
bool write_session_manifest(
    const std::string&              session_id,
    const std::string&              target_exe,
    std::uint32_t                   pid,
    const std::vector<FrameRecord>& frames,
    const std::filesystem::path&    out_path);

} // namespace openripper::exporters
