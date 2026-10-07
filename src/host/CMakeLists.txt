# xbox_host -- the host shell off Windows (host.h): the main-thread event
# loop src/platform's host_main_posix runs, and the game window the D3D8
# replacement draws into. Windows keeps its Win32 window (src/hle/hle_d3d8.c)
# and does not build this.

add_library(xbox_host STATIC
    host_sdl.c
    host_mac.c
)
target_include_directories(xbox_host PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(xbox_host PUBLIC SDL3::SDL3)
find_package(Threads REQUIRED)
target_link_libraries(xbox_host PUBLIC Threads::Threads)
if(APPLE)
    target_link_libraries(xbox_host PUBLIC "-framework AppKit" "-framework Foundation" objc)
endif()
target_compile_options(xbox_host PRIVATE -Wall -Wextra -Wno-unused-parameter)
