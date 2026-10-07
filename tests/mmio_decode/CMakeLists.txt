# mmio_decode -- the x86-64 trapped-access decoder (src/platform/mmio_decode.h)
# on hand-encoded instructions: lengths, widths, immediates, flags, refusals.
# Pure: it edits a register struct (CONTEXT on Windows, mmio_x86_ctx
# elsewhere) and calls two recording callbacks, so it runs anywhere the host
# is x86-64 (Windows and Linux CI); on another CPU it reports itself skipped.
# Header-only; no library, no game files.

add_executable(mmio_decode_test test_main.c)
target_include_directories(mmio_decode_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/platform
)
if(MSVC)
    target_compile_definitions(mmio_decode_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
endif()

add_test(NAME mmio_decode COMMAND mmio_decode_test)
