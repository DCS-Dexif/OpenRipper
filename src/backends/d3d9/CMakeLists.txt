add_library(OpenRipper_d3d9 SHARED
    dllmain.cpp
    hooks_d3d9.cpp
    capture_d3d9.cpp
)

target_link_libraries(OpenRipper_d3d9 PRIVATE
    openripper_core
    openripper_exporters
    minhook
    d3d9
    dxguid
    user32
)

openripper_apply_common_flags(OpenRipper_d3d9)

set_target_properties(OpenRipper_d3d9 PROPERTIES
    OUTPUT_NAME "OpenRipper_d3d9"
    PREFIX      ""
)
