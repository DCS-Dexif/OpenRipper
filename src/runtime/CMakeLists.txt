# Runtime: out-of-process helpers (CreateProcess, remote LoadLibraryW).
# Linked into the CLI and any future GUI launcher.

add_library(openripper_runtime STATIC
    injector.cpp
)

target_link_libraries(openripper_runtime
    PUBLIC openripper_core
)

openripper_apply_common_flags(openripper_runtime)
