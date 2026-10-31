cmake_minimum_required(VERSION 3.20)
if(NOT TARGET xbox_d3d8)
    project(d3d8_smoke_tests C)
    set(CMAKE_C_STANDARD 11)
    add_subdirectory(../.. runtime)
endif()

# d3d8_smoke - tests for the xbox_d3d8 layer.
#   d3d8_smoke         pure format layer (d3d8_resources.c with accessor stubs, no GPU)
#   d3d8_states_smoke  render-state -> pipeline-key translation (d3d8_states.c, no GPU)
#   d3d8_gamma         gamma ramp round trip and the present-time pass (needs a GPU device)
#   d3d8_a8            A8 textures through the fixed-function path (needs a GPU device)
# The GPU tests use a hidden window (RECOMP_HIDE_WINDOW=1).

add_executable(d3d8_smoke
    ${CMAKE_CURRENT_SOURCE_DIR}/test_main.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/d3d/d3d8_resources.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/gfx/gpu_device.c
)
target_include_directories(d3d8_smoke PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/d3d
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src
)
target_link_libraries(d3d8_smoke PRIVATE platform SDL3::SDL3)
if(MSVC)
    target_compile_options(d3d8_smoke PRIVATE /W3)
endif()

add_executable(d3d8_states_smoke
    test_states.c
    ../../src/d3d/d3d8_states.c
)
target_include_directories(d3d8_states_smoke PRIVATE ../../src/d3d ../../src)
target_link_libraries(d3d8_states_smoke PRIVATE platform SDL3::SDL3)

add_executable(d3d8_gamma test_gamma.c)
target_link_libraries(d3d8_gamma PRIVATE xbox_d3d8)
xbox_copy_dxc(d3d8_gamma)

add_executable(d3d8_a8 test_a8.c)
target_link_libraries(d3d8_a8 PRIVATE xbox_d3d8)
xbox_copy_dxc(d3d8_a8)

enable_testing()
add_test(NAME d3d8_smoke COMMAND d3d8_smoke)
add_test(NAME d3d8_states_smoke COMMAND d3d8_states_smoke)
add_test(NAME d3d8_gamma COMMAND d3d8_gamma)
add_test(NAME d3d8_a8 COMMAND d3d8_a8)
