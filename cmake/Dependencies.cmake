# -----------------------------------------------------------------------------
# Dependencies.cmake
#
# Pulls third-party libraries via FetchContent so a fresh clone builds with
# nothing more than CMake + a Windows SDK + a C++20 toolchain. vcpkg / Conan
# remain options for downstream packagers but are not required.
# -----------------------------------------------------------------------------

include(FetchContent)
set(FETCHCONTENT_QUIET FALSE)

# -----------------------------------------------------------------------------
# MinHook — small, BSD-2-licensed inline hooking library (x86/x64).
#   https://github.com/TsudaKageyu/minhook
#
# Used by every Windows backend to patch vtable entries (Present, Draw*, etc.)
# without touching the import table of the host process.
# -----------------------------------------------------------------------------
FetchContent_Declare(
    minhook
    GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
    GIT_TAG        master
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(minhook)

# -----------------------------------------------------------------------------
# stb_image_write — single-header PNG writer (public-domain / MIT, GPLv3-compatible).
#   https://github.com/nothings/stb
#
# Only stb_image_write.h is used; the implementation is activated in exactly
# one TU (src/exporters/png_exporter.cpp) via STB_IMAGE_WRITE_IMPLEMENTATION.
# -----------------------------------------------------------------------------
FetchContent_Declare(
    stb
    GIT_REPOSITORY https://github.com/nothings/stb.git
    GIT_TAG        master
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(stb)

add_library(stb_image_write INTERFACE)
target_include_directories(stb_image_write INTERFACE "${stb_SOURCE_DIR}")
