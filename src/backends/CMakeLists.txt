# -----------------------------------------------------------------------------
# Backends. One subdirectory per graphics API; each subdirectory builds an
# OpenRipper_<api>.dll loaded into the target process by the injector.
# -----------------------------------------------------------------------------

if(OPENRIPPER_BUILD_D3D11)
    add_subdirectory(d3d11)
endif()

if(OPENRIPPER_BUILD_D3D12)
    add_subdirectory(d3d12)
endif()

if(OPENRIPPER_BUILD_D3D9)
    add_subdirectory(d3d9)
endif()

# Reserved for later stages:
# if(OPENRIPPER_BUILD_OPENGL) add_subdirectory(opengl) endif()
# if(OPENRIPPER_BUILD_VULKAN) add_subdirectory(vulkan) endif()
