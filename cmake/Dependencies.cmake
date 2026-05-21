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
