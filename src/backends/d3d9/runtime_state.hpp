#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <string>

namespace openripper::backends::d3d9 {

inline std::filesystem::path g_output_dir{"captures"};

inline std::atomic<std::uint64_t> g_capture_frame_target{
    std::numeric_limits<std::uint64_t>::max()};

inline std::atomic<std::uint32_t> g_freeze_frames_remaining{0};
inline std::uint32_t              g_freeze_count{1};
inline bool                       g_time_freeze_on_rip{false};
inline bool                       g_flip_winding{false};
inline bool                       g_dedup{false};

inline std::string g_session_id;
inline std::string g_target_exe_name;

} // namespace openripper::backends::d3d9
