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
target_link_libraries(xbox_video PUBLIC xbox_input xbox_nv2a)
if(TARGET xbox_d3d8)
    target_link_libraries(xbox_video PUBLIC xbox_d3d8)
endif()
target_link_libraries(xbox_video PUBLIC platform)
if(WIN32)
    target_link_libraries(xbox_video PUBLIC ole32 dbghelp winmm)
endif()

# The renderer's HLSL, one source for both renderers (see nv2a_bindings.hlsli).
include(${PROJECT_SOURCE_DIR}/cmake/Nv2aHlsl.cmake)
set(_hlsl_dir ${NV2A_HLSL_DIR})
set(_gen ${CMAKE_CURRENT_BINARY_DIR}/generated)

# The host renderer and window behind nv2a_d3d11.h (the renderer contract) and
# fb_window.h (the window contract). D3D11 is the bundled implementation;
# EXTERNAL leaves both to the embedding project (an SDL3 GPU renderer, say),
# which links its own target providing the nv2a_d3d_* and fbw_* functions.
if(XBOXRECOMP_RENDERER STREQUAL "D3D11")
    # nv2a_d3d11.c compiles the parts, in order, at run time.
    target_sources(xbox_video PRIVATE fb_window_win32.c nv2a_d3d11.c)
    xbr_embed_nv2a_hlsl(xbox_video)
endif()

target_link_libraries(xbox_video PUBLIC xbox::ffmpeg)
