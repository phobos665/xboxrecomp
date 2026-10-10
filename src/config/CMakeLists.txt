# Player settings: the file the launcher writes and the runtime reads.
#
# Deliberately depends on nothing. The d3d backend, the kernel and the HLE
# all read settings, so anything this linked would be linked by all three.

add_library(xbox_config STATIC
    recomp_config.c
)

target_include_directories(xbox_config PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

if(MSVC)
    target_compile_options(xbox_config PRIVATE /W4 $<$<NOT:$<CONFIG:Debug>>:/O2>)
else()
    target_compile_options(xbox_config PRIVATE -Wall -Wextra -O2)
endif()

# What widescreen means for the title being built, said once in its
# CMakeLists, after the add_subdirectory that brings the toolkit in:
#
#     recomp_title_widescreen(NATIVE)                  # has a 16:9 mode
#     recomp_title_widescreen(HOR_PLUS REGISTER 60)    # has none
#
# The player has one widescreen switch. Whether that is the console flag
# alone or the flag and a wider camera is a fact about the game, and so is
# the vertex constant register its projection is in (REGISTER, optional for
# either mode; without it the register is 60). See recomp_config.h,
# "widescreen", for how it is resolved and what a title that says nothing
# gets. It is compiled into this library because the game and its launcher
# both link it, so the two cannot be told different things; a title that
# would rather say it in code calls recomp_widescreen_title.
#
# A title where widescreen cannot be right at all says so to its launcher
# instead: recomp_add_launcher(... NO_WIDESCREEN).
function(recomp_title_widescreen MODE)
    cmake_parse_arguments(_wide "" "REGISTER" "" ${ARGN})
    if(MODE STREQUAL "NATIVE")
        set(_mode 1)                                # RECOMP_WIDE_NATIVE
    elseif(MODE STREQUAL "HOR_PLUS")
        set(_mode 2)                                # RECOMP_WIDE_HOR_PLUS
    else()
        message(FATAL_ERROR
            "recomp_title_widescreen: \"${MODE}\" is neither NATIVE nor HOR_PLUS")
    endif()
    if(_wide_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR
            "recomp_title_widescreen: unexpected \"${_wide_UNPARSED_ARGUMENTS}\"")
    endif()
    target_compile_definitions(xbox_config PRIVATE RECOMP_TITLE_WIDESCREEN=${_mode})
    if(DEFINED _wide_REGISTER)
        if(NOT _wide_REGISTER MATCHES "^[0-9]+$" OR _wide_REGISTER GREATER 191)
            message(FATAL_ERROR
                "recomp_title_widescreen: REGISTER ${_wide_REGISTER} is not 0..191")
        endif()
        target_compile_definitions(xbox_config PRIVATE
            RECOMP_TITLE_PROJECTION_REGISTER=${_wide_REGISTER})
    endif()
endfunction()

add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../../tests/config_smoke
                 ${CMAKE_CURRENT_BINARY_DIR}/config_smoke)
