# mmio_decode_a64 -- the AArch64 trapped-access emulator against the hardware.
#
# Runs ~100 assembled load/store forms and a block of C compiled like the
# lifted code twice, on plain memory and on a PROT_NONE alias whose every
# access faults and is completed by src/platform/mmio_decode_a64.h, and
# requires identical registers and memory. arm64 POSIX only (it JITs a stub);
# elsewhere the executable reports itself skipped. Header-only: no library.

add_executable(mmio_decode_a64_test test_main.c)
target_include_directories(mmio_decode_a64_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/platform
)

add_test(NAME mmio_decode_a64 COMMAND mmio_decode_a64_test)
