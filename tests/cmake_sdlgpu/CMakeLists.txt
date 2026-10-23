cmake_minimum_required(VERSION 3.20)
project(sdlgpu_helper_test C)

# From the toolkit root: cmake -S tests/cmake_sdlgpu -B build/cmake_sdlgpu
# Then: cmake --build build/cmake_sdlgpu --config Release

# Model a parent application that already provides SDL but not DXC. No GPU or
# SDL binary is needed: this checks the shared helper's shader build graph.
add_library(SDL3::SDL3 INTERFACE IMPORTED)
option(XBR_TEST_SHADER_OVERRIDE "Compile a caller-owned shader fragment" OFF)
if(XBR_TEST_SHADER_OVERRIDE)
    set(XBOXRECOMP_NV2A_SHADER_DIR "${CMAKE_CURRENT_BINARY_DIR}/port-shaders")
    file(MAKE_DIRECTORY "${XBOXRECOMP_NV2A_SHADER_DIR}")
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/../../src/video/shaders/nv2a_y16d.hlsl" _shader)
    file(CONFIGURE OUTPUT "${XBOXRECOMP_NV2A_SHADER_DIR}/nv2a_y16d.hlsl"
        CONTENT "/* XBR_PORT_SHADER_TEST */\n${_shader}" @ONLY)
endif()
include(../../cmake/Nv2aSdlgpu.cmake)
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/probe.c" "int main(void) { return 0; }\n")
add_executable(sdlgpu_helper_test "${CMAKE_CURRENT_BINARY_DIR}/probe.c")
xbr_nv2a_sdlgpu_target(sdlgpu_helper_test)
if(NOT XBOXRECOMP_DXC)
    message(FATAL_ERROR "An existing SDL target must not suppress DXC setup")
endif()

# Compile the selected fragments through both shader build paths.
xbr_embed_nv2a_hlsl(sdlgpu_helper_test)
enable_testing()
add_test(NAME selected_shader_sources COMMAND ${CMAKE_COMMAND}
    "-DGEN=${CMAKE_CURRENT_BINARY_DIR}/sdlgpu_helper_test_generated"
    "-DROOT=${CMAKE_CURRENT_SOURCE_DIR}/../.."
    "-DPORT=${XBOXRECOMP_NV2A_SHADER_DIR}"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/verify_shader_sources.cmake")
