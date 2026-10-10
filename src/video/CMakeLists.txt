# Shared FMV player: Media Foundation decode onto a D3D8 texture.
add_library(xbox_video STATIC
    video_player.c
    video_pump.c
    fb_present.c
    nv2a_d3d11.c
    spike_sampler.c
)
target_include_directories(xbox_video PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)
# xbox_input: the windows record keys in its key table (keyboard.c).
# xbox_nv2a: the window scans out the PVIDEO overlay (nv2a_mmio_hook.c).
target_link_libraries(xbox_video PUBLIC xbox_d3d8 xbox_input xbox_nv2a)
if(WIN32)
    target_link_libraries(xbox_video PUBLIC mfplat mfreadwrite mfuuid mf ole32 dbghelp winmm)
endif()
