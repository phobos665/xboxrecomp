# Replays the synthetic capture (tests/d3d8_capture, --write) through the
# real renderer and checks the pictures. Run by ctest as
#   cmake -DCAPTURE_TEST=<exe> -DREPLAY=<exe> -DWORK=<dir> -P run.cmake
#
# What it proves: the whole renderer -- src/d3d, the rhi.h backend, DXC, the
# headless swap chain -- draws a frame with no window and no game data, the
# same frame three times over byte for byte, and the frame has the colours
# the capture describes where it describes them. A machine with no Vulkan
# device skips (SKIP_REGULAR_EXPRESSION in CMakeLists.txt), it does not fail.

file(REMOVE_RECURSE ${WORK})
file(MAKE_DIRECTORY ${WORK})

execute_process(COMMAND ${CAPTURE_TEST} --write ${WORK}/synthetic.d3dcap
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE out)
if(rc)
    message(FATAL_ERROR "writing the synthetic capture failed (${rc}): ${out}")
endif()

# An empty settings directory and no shader cache: nothing from the machine's
# own runs may change the picture.
set(ENV{XDG_CONFIG_HOME} ${WORK}/config)
set(ENV{APPDATA} ${WORK}/config)
set(ENV{RECOMP_SHADER_CACHE} 0)
execute_process(COMMAND ${REPLAY} ${WORK}/synthetic.d3dcap --out ${WORK}/syn
                        --loops 3 --dump-every --present
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE out)
message("${out}")
if(rc)
    message(FATAL_ERROR "d3d8_replay failed (${rc})")
endif()
foreach(f syn000.bmp syn001.bmp syn002.bmp syn000_present.bmp syn002_present.bmp)
    if(NOT EXISTS ${WORK}/${f})
        message(FATAL_ERROR "d3d8_replay wrote no ${f}")
    endif()
endforeach()

# Every loop starts from the capture's snapshot, so every loop's image is the same.
foreach(n 001 002)
    file(SHA256 ${WORK}/syn000.bmp a)
    file(SHA256 ${WORK}/syn${n}.bmp b)
    if(NOT a STREQUAL b)
        message(FATAL_ERROR "loop ${n} drew a different image from loop 000")
    endif()
endforeach()

# 24-bit bottom-up BMP, 640x480: pixel (x, y) is BGR at 54 + (479 - y) * 1920 + x * 3.
function(expect_pixel file x y r g b what)
    math(EXPR off "54 + (479 - ${y}) * 1920 + ${x} * 3")
    file(READ ${file} hex OFFSET ${off} LIMIT 3 HEX)
    string(SUBSTRING ${hex} 0 2 hb)
    string(SUBSTRING ${hex} 2 2 hg)
    string(SUBSTRING ${hex} 4 2 hr)
    foreach(c r g b)
        math(EXPR got "0x${h${c}}")
        math(EXPR d "${got} - ${${c}}")
        if(d GREATER 2 OR d LESS -2)
            message(FATAL_ERROR "${file}: ${what} at ${x},${y}: ${c} is ${got}, expected ${${c}} "
                                "(pixel #${hex}, BGR)")
        endif()
    endforeach()
endfunction()

foreach(img syn000.bmp syn000_present.bmp)
    # The clear, 0xFF203060.
    expect_pixel(${WORK}/${img} 10 10 0x20 0x30 0x60 "the clear colour")
    expect_pixel(${WORK}/${img} 320 400 0x20 0x30 0x60 "the clear colour")
    # Left: the fixed-function quad over a LIN_A8R8G8B8 checker. Linear
    # textures are texel-addressed on the NV2A (shadow-mode.md, Future
    # Perfect item 5), so its 0..1 coordinates cover the first texel only,
    # which is red, and point sampling keeps it red.
    expect_pixel(${WORK}/${img} 160 160 255 0 0 "the fixed-function quad")
    # Right: the vertex program's quad, orange from the NORMPACKED3 normal
    # through the combiners.
    expect_pixel(${WORK}/${img} 480 160 255 127 0 "the vertex program quad")
    # Below the left quad: the screen as it was after that quad, copied into
    # a 64x64 texture (a v8 SCREEN_COPY chunk) and drawn at 40..280 x
    # 300..460. The red square lands at about x 55-145, y 313-393 of it;
    # around it is the clear. Black here means the copy never happened.
    expect_pixel(${WORK}/${img} 100 350 255 0 0 "the screen copy's red square")
    expect_pixel(${WORK}/${img} 250 350 0x20 0x30 0x60 "the screen copy's clear")
endforeach()
message("d3d8_replay: the synthetic frame replays, identically each loop, with its colours")
