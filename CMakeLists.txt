cmake_minimum_required(VERSION 3.20)
project(xboxrecomp VERSION 0.8.0 LANGUAGES C CXX)

# Xbox Static Recompilation Runtime Library
# Provides link-time libraries for statically recompiled Xbox games.
# Pattern: extract emulator subsystems (xemu) into reusable static libraries.
#
# Libraries:
#   xbox_kernel    - Xbox kernel replacement layer (Xbox kernel ordinals → Win32)
#   xbox_d3d8      - D3D8 → D3D11 graphics compatibility
#   xbox_dsound    - DirectSound → software mixer compatibility
#   xbox_input     - Xbox gamepad → XInput compatibility
#   xbox_apu       - MCPX APU audio emulation (extracted from xemu)
#   xbox_nv2a      - NV2A GPU emulation (extracted from xemu)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Common definitions for all targets
add_compile_definitions(
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

# An implicit declaration is an error, everywhere, in every configuration.
#
# C89's rule is that an undeclared function returns int, and on this target
# that silently truncates a 64-bit return value to 32 bits and sign extends
# it. Seven files used getenv, malloc or calloc without <stdlib.h> and so
# received corrupted pointers: RECOMP_WATCHDOG_SECS faulted on
# 0xFFFFFFFFA94859D5 about one run in three and was inert the rest of the
# time, and two D3D8 draw paths were allocating through a truncated malloc.
#
# It was a warning the whole time (C4013, alongside C4047 for the assignment).
# A warning among hundreds in a build this size is not a signal, so it is an
# error now. C4047 stays a warning: it fires on legitimate casts elsewhere.
if(MSVC)
    add_compile_options(/we4013)
else()
    add_compile_options(-Werror=implicit-function-declaration)
endif()

# Option to build examples

# ── Runtime Libraries ─────────────────────────────────────────

# Platform: Win32/NT type vocabulary + POSIX implementation of generic
# Win32 host primitives. Carries no Xbox semantics; provides the foundation
# the kernel HLE and the graphics/audio/input layers build on.
add_subdirectory(src/platform)

# Core: Xbox kernel replacement (memory layout, file I/O, threading, sync)
add_subdirectory(src/kernel)

# Graphics: D3D8 → D3D11 compatibility layer
add_subdirectory(src/d3d)

# Audio: DirectSound → software mixer compatibility
add_subdirectory(src/audio)

# Audio: MCPX APU hardware emulation (voice processor, DSP)
add_subdirectory(src/apu)

# GPU: NV2A hardware emulation (register handlers, MMIO, push buffer)
add_subdirectory(src/nv2a)

# USB: MCPX OHCI host controllers. A title's XAPI is statically linked and
# drives these registers directly, so a pad only exists if they do.
add_subdirectory(src/usb)

# Input: Xbox gamepad → XInput
add_subdirectory(src/input)

# Shared FMV player (Media Foundation -> D3D8 texture)
add_subdirectory(src/video)

# XDK functions replaced by name (tools/recomp/hle.py)
add_subdirectory(src/hle)

# ── Umbrella target ───────────────────────────────────────────

# Convenience target that links all runtime libraries
add_library(xboxrecomp INTERFACE)
target_link_libraries(xboxrecomp INTERFACE
    xbox_kernel
    xbox_d3d8
    xbox_dsound
    xbox_input
    xbox_usb
    xbox_apu
    xbox_nv2a
    xbox_video
    xbox_hle
)

