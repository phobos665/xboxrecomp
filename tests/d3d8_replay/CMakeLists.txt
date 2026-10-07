# d3d8_replay -- the synthetic capture drawn by the real renderer.
#
# Off Windows the replay tool opens no window (a headless Vulkan swap chain),
# so this can run under ctest; on Windows it would open one, so it is not
# registered there. Needs a Vulkan device: without one (no loader, no ICD, a
# build without the Vulkan backend) it is reported as skipped.

add_test(NAME d3d8_replay
         COMMAND ${CMAKE_COMMAND}
                 -DCAPTURE_TEST=$<TARGET_FILE:d3d8_capture_test>
                 -DREPLAY=$<TARGET_FILE:d3d8_replay>
                 -DWORK=${CMAKE_CURRENT_BINARY_DIR}/work
                 -P ${CMAKE_CURRENT_SOURCE_DIR}/run.cmake)
set_tests_properties(d3d8_replay PROPERTIES
    SKIP_REGULAR_EXPRESSION "no Vulkan loader;no Vulkan device;has no renderer backend;nothing to present to"
    TIMEOUT 120)
