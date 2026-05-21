# CLI: OpenRipper.exe — out-of-process launcher / injector front-end.

add_executable(OpenRipper
    main.cpp
)

target_link_libraries(OpenRipper
    PRIVATE openripper_core
            openripper_runtime
)

openripper_apply_common_flags(OpenRipper)

set_target_properties(OpenRipper PROPERTIES
    OUTPUT_NAME "OpenRipper"
)
