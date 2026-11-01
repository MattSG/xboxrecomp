# Xbox D3D8 Compatibility Layer
#   Backed by SDL_GPU (SDL3): Direct3D 12, Vulkan and Metal from one code path.
#   Shaders are generated as HLSL while the game runs and compiled through
#   SDL_shadercross (see src/gfx/gpu_shader.c).

add_library(xbox_d3d8 STATIC
    d3d8_device.c
    d3d8_gamma.c
    d3d8_resources.c
    d3d8_shaders.c
    d3d8_states.c
    d3d8_combiners.c
    d3d8_vsh.c
    d3d8_pipeline.c
    d3d8_clear.c
    ../gfx/gpu_device.c
    ../gfx/gpu_shader.c
    ../gfx/sdl_events.c
)

target_include_directories(xbox_d3d8 PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)
target_link_libraries(xbox_d3d8 PUBLIC platform xbox_input SDL3::SDL3
    SDL3_shadercross::SDL3_shadercross-static)

if(MSVC)
    target_compile_options(xbox_d3d8 PRIVATE /W4 /O2)
else()
    target_compile_options(xbox_d3d8 PRIVATE -Wall -Wextra -Wno-unused-parameter -O2)
endif()

# Tests for this layer (also buildable on their own: tests/d3d8_smoke).
option(XBOXRECOMP_BUILD_D3D8_TESTS "Build the xbox_d3d8 smoke tests" OFF)
if(XBOXRECOMP_BUILD_D3D8_TESTS)
    add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../../tests/d3d8_smoke
                     ${CMAKE_CURRENT_BINARY_DIR}/d3d8_smoke)
endif()
