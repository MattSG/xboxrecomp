cmake_minimum_required(VERSION 3.20)
project(nv2a_surface_sync C)
enable_testing()
if(WIN32)
    include(../../cmake/Nv2aHlsl.cmake)   # the renderer's embedded HLSL
    add_executable(nv2a_surface_sync test_surface_sync.c)
    target_include_directories(nv2a_surface_sync PRIVATE ../../src ../../src/video)
    target_link_libraries(nv2a_surface_sync PRIVATE d3d11 dxgi dxguid d3dcompiler)
    foreach(samples 1 4)
        foreach(case initial_snapshot initial_pixels initial_texture initial_srv initial_constants
                     dirty_snapshot dirty_mask dirty_pixels dirty_texture dirty_srv mask_texture mask_srv
                     dirty_constants concurrent_initial concurrent_dirty padded16 swizzled unchanged multiple_surfaces idle_scanout)
            add_test(NAME nv2a_surface_${case}_${samples}x COMMAND nv2a_surface_sync ${case} ${samples})
        endforeach()
    endforeach()
    xbr_embed_nv2a_hlsl(nv2a_surface_sync)
endif()

# The SDL3 GPU renderer's surface sync, on every platform its backend builds on.
include(../../cmake/Nv2aSdlgpu.cmake)
add_subdirectory(../../src/platform ${CMAKE_CURRENT_BINARY_DIR}/platform)
add_executable(nv2a_surface_sync_sdlgpu test_surface_sync_sdlgpu.c ../../src/video/fb_window_sdl.c)
xbr_nv2a_sdlgpu_target(nv2a_surface_sync_sdlgpu)
target_link_libraries(nv2a_surface_sync_sdlgpu PRIVATE platform)
if(NOT MSVC)
    target_compile_options(nv2a_surface_sync_sdlgpu PRIVATE -fwrapv -fno-strict-aliasing)
endif()
foreach(case initial_snapshot initial_pixels initial_texture initial_constants
             dirty_snapshot dirty_mask dirty_pixels dirty_texture dirty_constants mask_texture
             concurrent_initial concurrent_dirty padded16 swizzled unchanged multiple_surfaces idle_scanout common)
    add_test(NAME nv2a_surface_sync_sdlgpu_${case} COMMAND nv2a_surface_sync_sdlgpu ${case})
endforeach()
