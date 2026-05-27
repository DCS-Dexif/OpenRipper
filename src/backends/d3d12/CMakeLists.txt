# D3D12 capture backend - OpenRipper_d3d12.dll.

add_library(OpenRipper_d3d12 SHARED
    dllmain.cpp
    hooks_d3d12.cpp
    state_d3d12.cpp
    capture_d3d12.cpp
    overlay_d3d12.cpp
)

target_link_libraries(OpenRipper_d3d12
    PRIVATE openripper_core
            openripper_exporters
            minhook
            d3d12
            d3d11
            dxgi
            d2d1
            dwrite
)

openripper_apply_common_flags(OpenRipper_d3d12)

set_target_properties(OpenRipper_d3d12 PROPERTIES
    OUTPUT_NAME "OpenRipper_d3d12"
    PREFIX      ""
)
