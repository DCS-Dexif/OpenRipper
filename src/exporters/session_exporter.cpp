// OpenRipper - src/exporters/session_exporter.cpp

#include "session_exporter.hpp"
#include "core/logger.hpp"

#include <fstream>

namespace openripper::exporters {

bool write_session_manifest(
    const std::string&              session_id,
    const std::string&              target_exe,
    std::uint32_t                   pid,
    const std::vector<FrameRecord>& frames,
    const std::filesystem::path&    out_path)
{
    std::ofstream f(out_path);
    if (!f) {
        OR_LOG_WARN("session: could not open '{}' for writing", out_path.string());
        return false;
    }

    f << "{\n"
      << "  \"session\": \"" << session_id << "\",\n"
      << "  \"target_exe\": \"" << target_exe << "\",\n"
      << "  \"pid\": " << pid << ",\n"
      << "  \"frames_captured\": [";

    for (std::size_t i = 0; i < frames.size(); ++i) {
        const auto& fr = frames[i];
        f << "\n    { \"frame\": " << fr.frame_id
          << ", \"draws\": " << fr.draw_count
          << ", \"manifest\": \"" << fr.manifest_file << "\" }";
        if (i + 1 < frames.size()) f << ',';
    }

    if (!frames.empty()) f << '\n' << "  ";
    f << "]\n}\n";

    OR_LOG_INFO("session: wrote '{}' ({} frame record(s))",
                out_path.filename().string(), frames.size());
    return true;
}

} // namespace openripper::exporters
