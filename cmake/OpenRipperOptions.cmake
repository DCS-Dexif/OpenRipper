# -----------------------------------------------------------------------------
# OpenRipperOptions.cmake
#
# Centralized compiler / linker flags applied to every OpenRipper target.
# Keeps individual CMakeLists.txt files focused on what they build, not how
# they build.
# -----------------------------------------------------------------------------

function(openripper_apply_common_flags target)
    target_compile_features(${target} PUBLIC cxx_std_20)

    if(MSVC)
        target_compile_options(${target} PRIVATE
            /W4
            /permissive-
            /Zc:__cplusplus
            /Zc:preprocessor
            /MP
            /utf-8
            $<$<CONFIG:Release>:/O2>
            $<$<CONFIG:Release>:/Oi>
        )
        target_compile_definitions(${target} PRIVATE
            _WIN32_WINNT=0x0A00      # Windows 10+
            WIN32_LEAN_AND_MEAN
            NOMINMAX
            _CRT_SECURE_NO_WARNINGS
            UNICODE
            _UNICODE
        )
    else()
        target_compile_options(${target} PRIVATE
            -Wall
            -Wextra
            -Wpedantic
            $<$<CONFIG:Release>:-O2>
        )
    endif()
endfunction()
