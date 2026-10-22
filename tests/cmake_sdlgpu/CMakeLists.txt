cmake_minimum_required(VERSION 3.20)
project(sdlgpu_helper_test C)

# From the toolkit root: cmake -S tests/cmake_sdlgpu -B build/cmake_sdlgpu
# Then: cmake --build build/cmake_sdlgpu --config Release

# Model a parent application that already provides SDL but not DXC. No GPU or
# SDL binary is needed: this checks the shared helper's shader build graph.
add_library(SDL3::SDL3 INTERFACE IMPORTED)
include(../../cmake/Nv2aSdlgpu.cmake)
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/probe.c" "int main(void) { return 0; }\n")
add_executable(sdlgpu_helper_test "${CMAKE_CURRENT_BINARY_DIR}/probe.c")
xbr_nv2a_sdlgpu_target(sdlgpu_helper_test)
if(NOT XBOXRECOMP_DXC)
    message(FATAL_ERROR "An existing SDL target must not suppress DXC setup")
endif()
