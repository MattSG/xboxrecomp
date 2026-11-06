cmake_minimum_required(VERSION 3.20)
project(xboxrecomp VERSION 0.14.0 LANGUAGES C)

# Xbox Static Recompilation Runtime Library
# Provides link-time libraries for statically recompiled Xbox games.
# Pattern: extract emulator subsystems (xemu) into reusable static libraries.
#
# Libraries:
#   xbox_kernel    - Xbox kernel replacement layer (147 imports → Win32)
#   xbox_d3d8      - D3D8 → D3D11 graphics compatibility
#   xbox_dsound    - DirectSound → software mixer compatibility
#   xbox_input     - Xbox gamepad → XInput compatibility
#   xbox_apu       - MCPX APU audio emulation (extracted from xemu)
#   xbox_nv2a      - NV2A GPU emulation (extracted from xemu)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Recompiled x86 code relies on what MSVC does anyway: signed arithmetic wraps
# like the CPU's, and guest memory is read through whatever pointer type an
# instruction implies. GCC and Clang optimise both away as undefined
# behaviour unless told not to.
if(NOT MSVC)
    add_compile_options(-fwrapv -fno-strict-aliasing)
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# Host backends. Each host-facing module has a function-level contract, so an
# embedding project can bring its own implementation (an SDL3 window, GPU
# renderer, pad and audio stack, say) and leave the bundled one out:
#   XBOXRECOMP_RENDERER=EXTERNAL     renderer + window: nv2a_d3d11.h, fb_window.h
#   XBOXRECOMP_APU_OUTPUT=EXTERNAL   audio device: apu_xaudio2.h (xa2_*)
#   XBOXRECOMP_BUILD_D3D8=OFF        D3D8 layer: an xbox_d3d8 target of its own
# D3D11 is the bundled renderer and window, available on Windows; elsewhere the
# embedding project provides them.
if(WIN32)
    set(_xbr_renderer_default D3D11)
else()
    set(_xbr_renderer_default EXTERNAL)
endif()
set(XBOXRECOMP_RENDERER "${_xbr_renderer_default}" CACHE STRING "Host renderer: D3D11 or EXTERNAL")
set_property(CACHE XBOXRECOMP_RENDERER PROPERTY STRINGS D3D11 EXTERNAL)
include(xbox_ffmpeg)

# Common definitions for all targets
add_compile_definitions(
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

# Option to build examples

# ── Runtime Libraries ─────────────────────────────────────────

# Platform: Win32/NT type vocabulary + POSIX implementation of generic
# Win32 host primitives. Carries no Xbox semantics; provides the foundation
# the kernel HLE and the graphics/audio/input layers build on.
add_subdirectory(src/platform)

# Core: Xbox kernel replacement (memory layout, file I/O, threading, sync)
add_subdirectory(src/kernel)

# Graphics: bundled D3D8 compatibility layer. Embedding projects that use the
# pushbuffer executor directly may omit it, or define their own xbox_d3d8
# target before adding xboxrecomp.
option(XBOXRECOMP_BUILD_D3D8 "Build the bundled xbox_d3d8 compatibility layer" ON)
if(NOT TARGET xbox_d3d8)
    if(XBOXRECOMP_BUILD_D3D8)
        add_subdirectory(src/d3d)
    elseif(WIN32)
        message(FATAL_ERROR
            "XBOXRECOMP_BUILD_D3D8=OFF on Windows requires a replacement xbox_d3d8 target")
    endif()
endif()

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

# Network: Ethernet controller, NAT and peer tunnel
add_subdirectory(src/net)

# Shared FMV player (Media Foundation -> D3D8 texture)
add_subdirectory(src/video)

# Host bring-up: MMIO routing to the APU/USB/NV2A models, window and device
add_subdirectory(src/host)

# ── Umbrella target ───────────────────────────────────────────

# Convenience target that links all runtime libraries
set(XBOXRECOMP_LIBS xbox_kernel)
if(TARGET xbox_d3d8)
    list(APPEND XBOXRECOMP_LIBS xbox_d3d8)
endif()
list(APPEND XBOXRECOMP_LIBS
    xbox_dsound
    xbox_input
    xbox_usb
    xbox_net
    xbox_apu
    xbox_nv2a
    xbox_video
    xbox_host
)
add_library(xboxrecomp INTERFACE)
if(UNIX AND NOT APPLE)
    # The archives call each other both ways (the APU reaches the kernel's
    # memory map, the kernel raises the APU's interrupt). GNU ld scans each
    # archive once, so it needs the group; MSVC and ld64 rescan on their own.
    target_link_libraries(xboxrecomp INTERFACE
        -Wl,--start-group ${XBOXRECOMP_LIBS} -Wl,--end-group)
else()
    target_link_libraries(xboxrecomp INTERFACE ${XBOXRECOMP_LIBS})
endif()

# ── C regression tests ────────────────────────────────────────
#
# The tests under tests/ are self-contained CMake projects that can also be
# built alone; listing them here puts them under the root build's ctest, so
# one `cmake --build && ctest` covers them alongside whatever else a
# workspace builds. The Python side (lifter unit tests, conformance) stays
# with pytest -- see CONTRIBUTING.md.
# Off when a game pulls the toolkit in with add_subdirectory.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(_xbr_tests_default ON)
else()
    set(_xbr_tests_default OFF)
endif()
option(XBOXRECOMP_BUILD_TESTS "Build the C regression tests" ${_xbr_tests_default})
if(XBOXRECOMP_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests/mmx_helpers)
    add_subdirectory(tests/xmm_helpers)
endif()

