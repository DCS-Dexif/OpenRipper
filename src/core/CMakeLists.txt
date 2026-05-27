# Core: logger, config loader, common capture data types.
# Header-friendly static library, linked into every other module.

add_library(openripper_core STATIC
    logger.cpp
    config.cpp
    hotkey.cpp
)

# Allow all OpenRipper modules to include "core/..." headers verbatim.
target_include_directories(openripper_core
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..
)

openripper_apply_common_flags(openripper_core)
