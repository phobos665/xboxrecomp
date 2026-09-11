# XDK functions replaced by name (see hle.h and tools/recomp/hle.py).
#
# Implementations are marked HLE_EXPORT(Name). tools.recomp scans this
# directory for the markers and generates the thunks that reach them, so a
# function added here is picked up at the next lift with no other edit.
#
# The DirectSound replacement (hle_dsound.c and the files it uses) is adapted
# from doaxbv-re (https://github.com/NoRain211/doaxbv-re), GPL-3.0.

add_library(xbox_hle STATIC
    hle_d3d8.c
    hle_dsound.c
    dsound_buffer_model.c
    xbox_adpcm.c
    audio_output_xaudio2.cpp
    hle_input.c
    input_model.c
    input_host.c
)
target_include_directories(xbox_hle PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(xbox_hle PUBLIC xbox_kernel)
if(WIN32)
    target_link_libraries(xbox_hle PRIVATE xaudio2 ole32 xinput9_1_0)
endif()
