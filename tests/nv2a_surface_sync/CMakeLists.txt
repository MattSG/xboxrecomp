cmake_minimum_required(VERSION 3.20)
project(nv2a_surface_sync C)
enable_testing()
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
