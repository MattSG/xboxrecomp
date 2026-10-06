# Shared FMV player: Media Foundation decode onto a D3D8 texture.
add_library(xbox_video STATIC
    video_player.c
    video_pump.c
    fb_present.c
    nv2a_d3d11.c
    nv2a_vsh_hlsl.c
)
target_include_directories(xbox_video PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)
target_link_libraries(xbox_video PUBLIC xbox_d3d8)
if(WIN32)
    target_link_libraries(xbox_video PUBLIC mfplat mfreadwrite mfuuid mf ole32)
endif()
