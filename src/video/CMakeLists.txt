# Video: the framebuffer display and window, the host renderer for the NV2A
# executor, and the shared FMV player (Media Foundation decode onto a D3D8
# texture). XBOXRECOMP_RENDERER (top-level CMakeLists.txt) picks the renderer.
add_library(xbox_video STATIC
    video_player.c
    video_pump.c
    fb_present.c
    spike_sampler.c
)
target_include_directories(xbox_video PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)
# xbox_input: the windows record keys in its key table (keyboard.c).
# xbox_nv2a: the window scans out the PVIDEO overlay (nv2a_mmio_hook.c).
target_link_libraries(xbox_video PUBLIC xbox_d3d8 xbox_input xbox_nv2a platform)
if(WIN32)
    target_link_libraries(xbox_video PUBLIC mfplat mfreadwrite mfuuid mf ole32 dbghelp winmm)
endif()

# The renderer's HLSL, one source for both renderers (see nv2a_bindings.hlsli).
include(${PROJECT_SOURCE_DIR}/cmake/Nv2aHlsl.cmake)
set(_hlsl_dir ${NV2A_HLSL_DIR})
set(_gen ${CMAKE_CURRENT_BINARY_DIR}/generated)

if(XBOXRECOMP_RENDERER STREQUAL "D3D11")
    # nv2a_d3d11.c compiles the parts, in order, at run time.
    target_sources(xbox_video PRIVATE fb_window_win32.c nv2a_d3d11.c)
    xbr_embed_nv2a_hlsl(xbox_video)
else()
    # Each SDL3 GPU shader is compiled ahead of time from its compile unit
    # (shaders/sdlgpu/) to SPIR-V (Vulkan) and, on Windows, DXIL (Direct3D 12),
    # embedded as byte arrays nv2a_sdlgpu.c includes.
    # Shader rules: cmake/Nv2aSdlgpu.cmake (shared with the backend's tests).
    include(${PROJECT_SOURCE_DIR}/cmake/Nv2aSdlgpu.cmake)
    xbr_sdlgpu_shaders(${_gen} _shader_headers)
    if(NV2A_DEPTH_VIEW_SHADER)
        target_compile_definitions(xbox_video PRIVATE XBOXRECOMP_DEPTH_VIEW_SHADER=1)
    endif()
    target_sources(xbox_video PRIVATE fb_window_sdl.c nv2a_sdlgpu.c ${_shader_headers})
    target_include_directories(xbox_video PRIVATE ${_gen})
    target_link_libraries(xbox_video PUBLIC SDL3::SDL3)
    target_compile_definitions(xbox_video PUBLIC XBOXRECOMP_SDL3=1)
endif()
