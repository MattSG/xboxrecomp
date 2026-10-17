cmake_minimum_required(VERSION 3.20)
project(nv2a_graphics_quality C)
enable_testing()

# SDL3 GPU backend: the panorama gate and the packed depth view.
include(../../cmake/Nv2aSdlgpu.cmake)
add_executable(nv2a_graphics_quality_sdlgpu test_graphics_quality_sdlgpu.c)
target_include_directories(nv2a_graphics_quality_sdlgpu PRIVATE ../../src ../../src/video)
xbr_nv2a_sdlgpu_target(nv2a_graphics_quality_sdlgpu)
foreach(case panorama gates depth)
    add_test(NAME nv2a_graphics_quality_sdlgpu_${case} COMMAND nv2a_graphics_quality_sdlgpu ${case})
endforeach()

if(NOT WIN32)
    message(STATUS "nv2a_graphics_quality: D3D11 renderer test, Windows only")
    return()
endif()

include(../../cmake/Nv2aHlsl.cmake)   # the renderer's embedded HLSL

add_executable(nv2a_graphics_quality test_graphics_quality.c)
target_include_directories(nv2a_graphics_quality PRIVATE ../../src ../../src/video)
target_link_libraries(nv2a_graphics_quality PRIVATE d3d11 dxgi dxguid d3dcompiler)
foreach(case panorama gates depth placement)
    add_test(NAME nv2a_graphics_quality_${case} COMMAND nv2a_graphics_quality ${case})
endforeach()
xbr_embed_nv2a_hlsl(nv2a_graphics_quality)
