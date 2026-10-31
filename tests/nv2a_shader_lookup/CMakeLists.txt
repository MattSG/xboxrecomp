cmake_minimum_required(VERSION 3.20)
project(nv2a_shader_lookup C)
enable_testing()

# SDL3 GPU backend: its pipeline cache (the counterpart of the D3D11 shader
# table) and the ring uploads of vertex data.
include(../../cmake/Nv2aSdlgpu.cmake)
add_executable(nv2a_shader_lookup_sdlgpu test_shader_lookup_sdlgpu.c)
target_include_directories(nv2a_shader_lookup_sdlgpu PRIVATE ../../src ../../src/video)
xbr_nv2a_sdlgpu_target(nv2a_shader_lookup_sdlgpu)
foreach(case pipelines rings)
    add_test(NAME nv2a_shader_lookup_sdlgpu_${case} COMMAND nv2a_shader_lookup_sdlgpu ${case})
endforeach()

if(NOT WIN32)
    message(STATUS "nv2a_shader_lookup: D3D11 renderer test, Windows only")
    return()
endif()

include(../../cmake/Nv2aHlsl.cmake)   # the renderer's embedded HLSL
foreach(target shader_lookup vertex_retry)
    add_executable(nv2a_${target} test_${target}.c)
    target_include_directories(nv2a_${target} PRIVATE ../../src ../../src/video)
    target_link_libraries(nv2a_${target} PRIVATE d3d11 dxgi dxguid d3dcompiler)
endforeach()
foreach(case lookup full queue)
    add_test(NAME nv2a_shader_${case} COMMAND nv2a_shader_lookup ${case})
endforeach()
foreach(case version_zero program constants ub rings)
    add_test(NAME nv2a_vertex_retry_${case} COMMAND nv2a_vertex_retry ${case})
endforeach()
xbr_embed_nv2a_hlsl(nv2a_shader_lookup)
xbr_embed_nv2a_hlsl(nv2a_vertex_retry)
