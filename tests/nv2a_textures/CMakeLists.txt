cmake_minimum_required(VERSION 3.20)
project(nv2a_textures C)
enable_testing()

# CPU decoding and mip generation: shared renderer code, run on every host.
add_executable(nv2a_textures_cpu test_textures_cpu.c)
target_include_directories(nv2a_textures_cpu PRIVATE ../../src ../../src/video ../../src/kernel)
foreach(case decode_equivalence mip_inplace)
    add_test(NAME nv2a_texture_${case} COMMAND nv2a_textures_cpu ${case})
endforeach()

# SDL3 GPU backend: the same texture-cache behaviour, read back from the GPU.
# Built wherever the SDL3 GPU renderer builds (always off Windows).
include(../../cmake/Nv2aSdlgpu.cmake)
add_executable(nv2a_textures_sdlgpu test_textures_sdlgpu.c)
target_include_directories(nv2a_textures_sdlgpu PRIVATE ../../src ../../src/video ../../src/kernel)
xbr_nv2a_sdlgpu_target(nv2a_textures_sdlgpu)
foreach(case srv_retry mip_retry palette_identity mip_chain)
    add_test(NAME nv2a_texture_sdlgpu_${case} COMMAND nv2a_textures_sdlgpu ${case})
endforeach()

if(NOT WIN32)
    message(STATUS "nv2a_textures: D3D11 GPU cases are Windows only")
    return()
endif()
include(../../cmake/Nv2aHlsl.cmake)   # the renderer's embedded HLSL
add_executable(nv2a_textures test_textures.c ../../src/kernel/kernel_hal.c)
target_include_directories(nv2a_textures PRIVATE ../../src ../../src/video ../../src/kernel)
if(NV2A_TEXTURE_RENDERER)
    target_compile_definitions(nv2a_textures PRIVATE
        NV2A_TEXTURE_RENDERER="${NV2A_TEXTURE_RENDERER}")
endif()
target_link_libraries(nv2a_textures PRIVATE d3d11 dxgi dxguid d3dcompiler)
foreach(case srv_retry mip_retry palette_identity palette_bounds refusals scanout_size native_bc mip_chain)
    add_test(NAME nv2a_texture_${case} COMMAND nv2a_textures ${case})
endforeach()
# Observing a failed/retried upload must preserve the same GPU pixels and
# cache publication contracts as normal rendering.
foreach(case srv_retry mip_retry palette_identity mip_chain)
    add_test(NAME nv2a_texture_${case}_trace COMMAND nv2a_textures ${case})
    set_tests_properties(nv2a_texture_${case}_trace PROPERTIES
        ENVIRONMENT "RECOMP_TEXTURE_STREAM_TRACE=1"
        PASS_REGULAR_EXPRESSION "\\[TEXTURE_STREAM\\].*event=new_upload")
endforeach()
xbr_embed_nv2a_hlsl(nv2a_textures)
