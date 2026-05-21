# -----------------------------------------------------------------------------
# src/CMakeLists.txt
#
# Module ordering:
#   core      -> base library, depended on by everyone.
#   exporters -> static lib used by backends to write captured assets.
#   runtime   -> out-of-process helpers (CreateProcess, remote LoadLibraryW).
#   cli       -> OpenRipper.exe.
#   backends  -> one shared library per graphics API.
# -----------------------------------------------------------------------------

add_subdirectory(core)
add_subdirectory(exporters)
add_subdirectory(runtime)

if(OPENRIPPER_BUILD_CLI)
    add_subdirectory(cli)
endif()

add_subdirectory(backends)
