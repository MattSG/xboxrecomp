cmake_minimum_required(VERSION 3.20)
project(recomp_kernel_geometry C)
set(CMAKE_C_STANDARD 11)
add_executable(recomp_kernel_geometry test_kernel_geometry.c
    ../../src/kernel/polyline.c
    ../../src/kernel/road_graph.c
    ../../src/kernel/vec3_rotate.c
    ../../src/kernel/kb_spline.c
    ../../src/kernel/guest_read.c)
target_include_directories(recomp_kernel_geometry PRIVATE ../../src ../../src/kernel)
target_link_libraries(recomp_kernel_geometry PRIVATE m)
enable_testing()
add_test(NAME recomp_kernel_geometry COMMAND recomp_kernel_geometry)
