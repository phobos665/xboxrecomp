# The mods overlay in the path layer (src/kernel/kernel_path.c), with no game:
# a temporary disc folder and mods folder, and the paths a title would open.
# Built from the one source file plus stubs, so it runs on Linux as well.

add_executable(kernel_path_mods
    test_mods.c
    ${CMAKE_CURRENT_LIST_DIR}/../../src/kernel/kernel_path.c)
target_include_directories(kernel_path_mods PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/../../src/kernel
    ${CMAKE_CURRENT_LIST_DIR}/../../src/config
    ${CMAKE_CURRENT_LIST_DIR}/../../src/platform
    ${CMAKE_CURRENT_LIST_DIR}/../../include
    ${CMAKE_CURRENT_LIST_DIR}/../../src)
if(WIN32)
    target_link_libraries(kernel_path_mods PRIVATE shell32)
endif()
add_test(NAME kernel_path_mods COMMAND kernel_path_mods)
