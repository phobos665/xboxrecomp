# apu_mixdown -- the DSP mixdown takes every bin the guest routed to.
#
# From upstream PR #67, adapted to this tree's layout: the tests here are
# subdirectories of the root project rather than standalone builds.
#
# Registered TWICE, on purpose. mcpx_apu_mixdown_all caches its getenv in a
# static, so one process can only ever exercise one arm -- and an arm that
# cannot fail is not a test. The off arm is the negative control: it must show
# the effect bins really are discarded without this change, or the on arm
# proves nothing.
add_executable(apu_mixdown_test test_main.c contiguous_stub.c)
target_include_directories(apu_mixdown_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/apu)
target_link_libraries(apu_mixdown_test PRIVATE xbox_apu)

add_test(NAME apu_mixdown_all COMMAND apu_mixdown_test)
set_tests_properties(apu_mixdown_all PROPERTIES
                     ENVIRONMENT "RECOMP_APU_MIXDOWN_ALL=1;APU_MIXDOWN_EXPECT=1")
add_test(NAME apu_mixdown_two_bins COMMAND apu_mixdown_test)
set_tests_properties(apu_mixdown_two_bins PROPERTIES
                     ENVIRONMENT "RECOMP_APU_MIXDOWN_ALL=0;APU_MIXDOWN_EXPECT=0")
