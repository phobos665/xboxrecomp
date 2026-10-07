cmake_minimum_required(VERSION 3.20)
project(platform C)

# Platform layer
#   xbox_winnt.h   - Win32 / NT type vocabulary (header only)
#   win32_compat.* - POSIX implementation of generic Win32 host primitives
#   host_timer.*   - a sub-millisecond timer on every host
#   recomp_fault.h - host faults (fault_win32.c: VEH; fault_posix.c: signals)
#   host_main.h    - what a title's main.c needs from the host
#                    (host_main_win32.c / host_main_posix.c)
#
# On Windows the real <windows.h> is used for the Win32 vocabulary, so the
# library carries only the host_* pieces that have a Windows half. On POSIX it
# also carries the Win32 primitive implementations.

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# src/, the directory this one lives in. Resolved relative to here rather than
# via CMAKE_SOURCE_DIR, which points at the *consuming* project's root when a
# game project pulls xboxrecomp in with add_subdirectory() -- the documented
# integration pattern -- and would send "platform/xbox_winnt.h" looking in the
# game's own src/ tree. PROJECT_SOURCE_DIR is no good either: this file
# declares its own project().
get_filename_component(XBOXRECOMP_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)

if(WIN32)
    add_library(platform STATIC
        host_timer.c
        fault_common.c
        fault_win32.c
        host_main_win32.c
    )
    # src/ only: consumers name these headers "platform/...".
    target_include_directories(platform PUBLIC ${XBOXRECOMP_SRC_DIR})
    # winmm: timeBeginPeriod, the vblank clock's fallback (host_timer.c).
    # dbghelp: the fault report's symbol names (fault_win32.c, host_main_win32.c).
    target_link_libraries(platform PUBLIC winmm dbghelp)
    if(MSVC)
        target_compile_options(platform PRIVATE /W4 /Zi)
        target_compile_definitions(platform PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    endif()
else()
    add_library(platform STATIC
        win32_compat.c
        posix_memory.c
        host_timer.c
        fault_common.c
        fault_posix.c
        host_main_posix.c
    )
    target_include_directories(platform PUBLIC
        ${XBOXRECOMP_SRC_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}
    )
    find_package(Threads REQUIRED)
    # dl: dladdr / dl_iterate_phdr for the fault report (host_main_posix.c).
    target_link_libraries(platform PUBLIC Threads::Threads ${CMAKE_DL_LIBS})
    target_compile_options(platform PRIVATE -Wall -Wextra -Wno-unused-parameter -O2)
endif()
