cmake_minimum_required(VERSION 3.20)
project(nv2a_textures C)
enable_testing()
add_executable(nv2a_textures test_textures.c ../../src/kernel/kernel_hal.c)
target_include_directories(nv2a_textures PRIVATE ../../src ../../src/video ../../src/kernel)
if(NV2A_TEXTURE_RENDERER)
    target_compile_definitions(nv2a_textures PRIVATE
        NV2A_TEXTURE_RENDERER="${NV2A_TEXTURE_RENDERER}")
endif()
target_link_libraries(nv2a_textures PRIVATE d3d11 dxgi dxguid d3dcompiler)
foreach(case srv_retry mip_retry palette_identity palette_bounds refusals scanout_size decode_equivalence native_bc mip_inplace mip_chain)
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
