# XDK functions replaced by name (see hle.h and tools/recomp/hle.py).
#
# Implementations are marked HLE_EXPORT(Name). tools.recomp scans this
# directory for the markers and generates the thunks that reach them, so a
# function added here is picked up at the next lift with no other edit.
#
# The DirectSound replacement (hle_dsound.c and the files it uses) is adapted
# from doaxbv-re (https://github.com/NoRain211/doaxbv-re), GPL-3.0.

if(MSVC)
    # /Zi so the PDB names this library's static functions in a sampled
    # profile (RECOMP_SAMPLE); see src/kernel/CMakeLists.txt.
    add_compile_options(/Zi)
endif()

add_library(xbox_hle STATIC
    hle_d3d8.c
    hle_d3d8_state.c
    hle_d3d8_texture.c
    hle_d3d8_vertex.c
    hle_d3d8_record.c
    hle_d3d8_interp.c
    d3d8_capture.c
    hle_dsound.c
    hle_dsound_stream.c
    dsound_buffer_model.c
    xbox_adpcm.c
    hle_input.c
    hle_xmv.c
    hle_xmv_play.c
    input_model.c
    # The host pads behind the replaced XAPI input, through src/input's
    # bindings (SDL3 or XInput), plus RECOMP_FAKE_INPUT / RECOMP_INPUT_SEQ.
    input_host.c
)

# Host audio. XAudio2 exists only on Windows. Elsewhere audio_output_none.c
# behaves as the XAudio2 path does when it opens no device
# (RECOMP_AUDIO_GAIN=0) until an SDL3 output replaces it.
if(WIN32)
    target_sources(xbox_hle PRIVATE audio_output_xaudio2.cpp)
else()
    target_sources(xbox_hle PRIVATE audio_output_none.c)
endif()
target_include_directories(xbox_hle PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(xbox_hle PUBLIC xbox_config)
target_link_libraries(xbox_hle PUBLIC xbox_kernel)
# Host device choice and per-control bindings (src/input/input_bindings.c),
# shared with the USB gamepad so one config file covers both input paths.
target_link_libraries(xbox_hle PUBLIC xbox_input)
if(WIN32)
    target_link_libraries(xbox_hle PRIVATE xaudio2 ole32 xinput9_1_0)
    # The D3D8 replacements drive the host renderer in src/d3d, and open a
    # window of their own for it.
    target_link_libraries(xbox_hle PRIVATE xbox_d3d8 xbox_video user32)
endif()
if(NOT WIN32)
    # The host shell (src/host): the main-thread loop a title's main.c runs,
    # and the game window. PUBLIC so a title gets it through this library.
    target_link_libraries(xbox_hle PUBLIC xbox_host)
endif()
