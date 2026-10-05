# A mod's tuning patches (src/kernel/mod_patches.c), against fake guest
# memory: the parser and the rules, with no game and no runtime.

add_executable(mod_patches_test
    test_patches.c
    ${CMAKE_CURRENT_LIST_DIR}/../../src/kernel/mod_patches.c)
target_compile_definitions(mod_patches_test PRIVATE MOD_PATCHES_NO_RUNTIME)
target_include_directories(mod_patches_test PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/../../src/kernel)
if(NOT MSVC)
    target_link_libraries(mod_patches_test PRIVATE m)
endif()
add_test(NAME mod_patches COMMAND mod_patches_test)

# The same, end to end through the path layer's mods folder and a directory
# scan (FindFirstFile on Windows, opendir elsewhere).
add_executable(mod_patches_runtime
    test_runtime.c
    ${CMAKE_CURRENT_LIST_DIR}/../../src/kernel/mod_patches.c
    ${CMAKE_CURRENT_LIST_DIR}/../../src/kernel/kernel_path.c)
target_include_directories(mod_patches_runtime PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/../../src/kernel
    ${CMAKE_CURRENT_LIST_DIR}/../../src/config
    ${CMAKE_CURRENT_LIST_DIR}/../../src/platform
    ${CMAKE_CURRENT_LIST_DIR}/../../include
    ${CMAKE_CURRENT_LIST_DIR}/../../src)
if(WIN32)
    target_link_libraries(mod_patches_runtime PRIVATE shell32)
else()
    target_link_libraries(mod_patches_runtime PRIVATE m)
endif()
add_test(NAME mod_patches_runtime COMMAND mod_patches_runtime)
