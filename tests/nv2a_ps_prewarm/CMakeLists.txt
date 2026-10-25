cmake_minimum_required(VERSION 3.20)
project(nv2a_ps_prewarm C)
enable_testing()

# SDL3 GPU backend: it has no specialised pixel shaders, so the warm-up
# switches must be inert there.
include(../../cmake/Nv2aSdlgpu.cmake)
add_executable(nv2a_ps_prewarm_sdlgpu test_ps_prewarm_sdlgpu.c)
target_include_directories(nv2a_ps_prewarm_sdlgpu PRIVATE ../../src ../../src/video)
xbr_nv2a_sdlgpu_target(nv2a_ps_prewarm_sdlgpu)
add_test(NAME nv2a_ps_prewarm_sdlgpu_no_specialisation
    COMMAND nv2a_ps_prewarm_sdlgpu no_specialisation)

if(NOT WIN32)
    message(STATUS "nv2a_ps_prewarm: D3D11 renderer test, Windows only")
    return()
endif()

include(../../cmake/Nv2aHlsl.cmake)   # the renderer's embedded HLSL

add_executable(nv2a_ps_prewarm test_main.c)
target_include_directories(nv2a_ps_prewarm PRIVATE ../../src ../../src/video)
target_link_libraries(nv2a_ps_prewarm PRIVATE d3d11 dxgi dxguid d3dcompiler)
foreach(case manifest scheduler persistence disabled cache_off prewarm_off invalid_variant gpu gpu_cache_failure constants)
    add_test(NAME nv2a_ps_prewarm_${case} COMMAND nv2a_ps_prewarm ${case})
endforeach()
xbr_embed_nv2a_hlsl(nv2a_ps_prewarm)
