#pragma once

namespace openripper::backends::d3d9 {
    bool install_hooks();
    void remove_hooks();
    void flush_session_manifest_now();
    void activate_capture_if_frame_zero();
} // namespace openripper::backends::d3d9
