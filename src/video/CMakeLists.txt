# Shared FMV player: Media Foundation decode onto a D3D8 texture.
add_library(xbox_video STATIC
    video_player.c
    video_pump.c
    fb_present.c
    xmv_demux.c
    xmv_decode.c
)
target_include_directories(xbox_video PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)
target_link_libraries(xbox_video PUBLIC xbox_d3d8)
if(WIN32)
    target_link_libraries(xbox_video PUBLIC mfplat mfreadwrite mfuuid mf ole32)
endif()

# XMV movies decode through FFmpeg, loaded at run time (xmv_decode.c): only the
# headers are needed here. Without them the movies are skipped as before.
# third_party/ffmpeg is the BtbN LGPL shared build, fetched rather than
# committed -- see docs/technical/video-playback.md.
set(XBOXRECOMP_FFMPEG_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/ffmpeg"
    CACHE PATH "FFmpeg shared build: include/ at build time, bin/ at run time")
get_filename_component(XBOXRECOMP_FFMPEG_DIR "${XBOXRECOMP_FFMPEG_DIR}" ABSOLUTE)
# The shared libraries are in bin/ in the Windows drop, lib/ in a Unix prefix.
set(XBR_FFMPEG_RUNTIME bin)
if(NOT WIN32)
    set(XBR_FFMPEG_RUNTIME lib)
endif()
set(XBR_FFMPEG_SYSTEM OFF)
if(APPLE AND NOT EXISTS "${XBOXRECOMP_FFMPEG_DIR}/include/libavcodec/avcodec.h")
    # macOS: Homebrew's FFmpeg (brew install ffmpeg), headers and dylibs of
    # one version, when there is no third_party/ffmpeg.
    foreach(prefix /opt/homebrew /usr/local)
        if(NOT XBR_FFMPEG_SYSTEM AND EXISTS "${prefix}/include/libavcodec/avcodec.h")
            set(XBOXRECOMP_FFMPEG_DIR "${prefix}")
            set(XBR_FFMPEG_SYSTEM ON)
        endif()
    endforeach()
endif()
if(EXISTS "${XBOXRECOMP_FFMPEG_DIR}/include/libavcodec/avcodec.h")
    if(XBR_FFMPEG_SYSTEM)
        # A whole prefix's headers (SDL, Vulkan, OpenSSL may be there too):
        # searched after everything else, so only FFmpeg's are found in it.
        target_compile_options(xbox_video PRIVATE -idirafter "${XBOXRECOMP_FFMPEG_DIR}/include")
    else()
        target_include_directories(xbox_video PRIVATE "${XBOXRECOMP_FFMPEG_DIR}/include")
    endif()
    target_compile_definitions(xbox_video PRIVATE XMV_HAVE_FFMPEG
        XMV_FFMPEG_BIN="${XBOXRECOMP_FFMPEG_DIR}/${XBR_FFMPEG_RUNTIME}")
    message(STATUS "xbox_video: XMV movies through FFmpeg at ${XBOXRECOMP_FFMPEG_DIR}")
else()
    message(STATUS "xbox_video: no FFmpeg headers at ${XBOXRECOMP_FFMPEG_DIR}; XMV movies are skipped")
endif()
