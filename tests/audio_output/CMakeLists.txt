# audio_output -- the SDL3 host audio output (src/hle/audio_output_sdl.c)
# keeps audio_output.h's contract: a muted device still clocks, 12 in flight,
# a refused chunk is taken later. Always muted (RECOMP_MUTE=1 forced). Prints
# SKIP and passes when the machine has no audio device.

add_executable(audio_output_clock
    audio_output_clock.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/hle/audio_output_sdl.c
)
target_include_directories(audio_output_clock PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../src/hle)
target_link_libraries(audio_output_clock PRIVATE SDL3::SDL3 m)
add_test(NAME audio_output_clock COMMAND audio_output_clock)
set_tests_properties(audio_output_clock PROPERTIES ENVIRONMENT "RECOMP_MUTE=1")
