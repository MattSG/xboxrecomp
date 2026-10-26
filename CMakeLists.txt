cmake_minimum_required(VERSION 3.20)
project(xboxrecomp VERSION 0.14.0 LANGUAGES C)

# Recompiled x86 code relies on what MSVC does anyway: signed arithmetic wraps
# like the CPU's, and guest memory is read through whatever pointer type an
# instruction implies. GCC and Clang optimise both away as undefined
# behaviour unless told not to.
if(NOT MSVC)
    add_compile_options(-fwrapv -fno-strict-aliasing)
endif()

# Xbox Static Recompilation Runtime Library
# Provides link-time libraries for statically recompiled Xbox games.
# Pattern: extract emulator subsystems (xemu) into reusable static libraries.
#
# Runtime modules and their transitive host dependencies are declared below.
# Link the xboxrecomp interface target rather than relying on a snapshot count.

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# The host renderer. D3D11 is Windows-only and the default there; SDL3_GPU
# (Vulkan, Direct3D 12 or Metal through SDL3) is the one choice elsewhere.
# SDL3_GPU also moves the window and its input to SDL3, and off Windows the
# audio output and game controllers too.
if(WIN32)
    set(_xbr_renderer_default D3D11)
else()
    set(_xbr_renderer_default SDL3_GPU)
endif()
set(XBOXRECOMP_RENDERER "${_xbr_renderer_default}" CACHE STRING "Host renderer: D3D11 or SDL3_GPU")
set_property(CACHE XBOXRECOMP_RENDERER PROPERTY STRINGS D3D11 SDL3_GPU)
if(XBOXRECOMP_RENDERER STREQUAL "SDL3_GPU")
    set(XBOXRECOMP_SDL3 ON)
    include(Sdl3)
    include(Dxc)
elseif(XBOXRECOMP_RENDERER STREQUAL "D3D11")
    if(NOT WIN32)
        message(FATAL_ERROR "XBOXRECOMP_RENDERER=D3D11 needs Windows; use SDL3_GPU")
    endif()
    set(XBOXRECOMP_SDL3 OFF)
else()
    message(FATAL_ERROR "XBOXRECOMP_RENDERER must be D3D11 or SDL3_GPU, not ${XBOXRECOMP_RENDERER}")
endif()
message(STATUS "xboxrecomp renderer: ${XBOXRECOMP_RENDERER}")
if(XBOXRECOMP_SDL3)
    add_compile_definitions(XBOXRECOMP_SDL3=1)
endif()

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

# Network: Ethernet controller, NAT and peer tunnel
add_subdirectory(src/net)

# Shared FMV player (Media Foundation -> D3D8 texture)
add_subdirectory(src/video)

# ── Umbrella target ───────────────────────────────────────────

# Convenience target that links all runtime libraries
set(XBOXRECOMP_LIBS
    xbox_kernel
    xbox_d3d8
    xbox_dsound
    xbox_input
    xbox_usb
    xbox_net
    xbox_apu
    xbox_nv2a
    xbox_video
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

