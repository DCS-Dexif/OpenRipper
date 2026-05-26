# D3D11 capture backend - OpenRipper_d3d11.dll.

add_library(OpenRipper_d3d11 SHARED
    dllmain.cpp
    hooks_d3d11.cpp
    state_d3d11.cpp
    capture_d3d11.cpp
    hotkey_d3d11.cpp
    overlay_d3d11.cpp
)

target_link_libraries(OpenRipper_d3d11
    PRIVATE openripper_core
            openripper_exporters
            minhook
            d3d11
            dxgi
            d2d1
            dwrite
)

openripper_apply_common_flags(OpenRipper_d3d11)

# Avoid the default 'lib' prefix some generators add on non-MSVC platforms;
# the injector resolves the DLL by its exact name.
set_target_properties(OpenRipper_d3d11 PROPERTIES
    OUTPUT_NAME "OpenRipper_d3d11"
    PREFIX      ""
)
