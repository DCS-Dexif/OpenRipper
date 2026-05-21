cmake_minimum_required(VERSION 3.21)

project(OpenRipper
    VERSION     0.1.0
    DESCRIPTION "Open-source 3D asset extraction toolkit for personal archival and preservation"
    LANGUAGES   CXX C
)

# -----------------------------------------------------------------------------
# Global options
# -----------------------------------------------------------------------------
option(OPENRIPPER_BUILD_D3D11  "Build the Direct3D 11 capture backend"     ON)
option(OPENRIPPER_BUILD_D3D12  "Build the Direct3D 12 capture backend"     OFF)
option(OPENRIPPER_BUILD_D3D9   "Build the Direct3D 9 capture backend"      OFF)
option(OPENRIPPER_BUILD_OPENGL "Build the OpenGL capture backend"          OFF)
option(OPENRIPPER_BUILD_VULKAN "Build the Vulkan capture backend"          OFF)
option(OPENRIPPER_BUILD_CLI    "Build the command-line launcher"           ON)
option(OPENRIPPER_BUILD_GUI    "Build the ImGui front-end"                 OFF)
option(OPENRIPPER_BUILD_TESTS  "Build unit tests"                          OFF)

# -----------------------------------------------------------------------------
# Standards / build defaults
# -----------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD          20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS        OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Default build type" FORCE)
endif()

# Place every artifact under build/bin and build/lib so the injector can find
# OpenRipper_<api>.dll next to OpenRipper.exe without extra search logic.
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)

include(OpenRipperOptions)
include(Dependencies)

add_subdirectory(src)

# -----------------------------------------------------------------------------
# Build summary
# -----------------------------------------------------------------------------
message(STATUS "OpenRipper ${PROJECT_VERSION}")
message(STATUS "  CXX compiler   : ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  D3D11 backend  : ${OPENRIPPER_BUILD_D3D11}")
message(STATUS "  D3D12 backend  : ${OPENRIPPER_BUILD_D3D12}")
message(STATUS "  D3D9  backend  : ${OPENRIPPER_BUILD_D3D9}")
message(STATUS "  OpenGL backend : ${OPENRIPPER_BUILD_OPENGL}")
message(STATUS "  Vulkan backend : ${OPENRIPPER_BUILD_VULKAN}")
message(STATUS "  CLI launcher   : ${OPENRIPPER_BUILD_CLI}")
message(STATUS "  GUI front-end  : ${OPENRIPPER_BUILD_GUI}")
