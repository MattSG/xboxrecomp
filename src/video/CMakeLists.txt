# Shared FMV player: FFmpeg decode onto a D3D8 texture, plus the SDL3
# framebuffer window. SDL3 has no decoder, hence FFmpeg on every host.
add_library(xbox_video STATIC
    video_player.c
    video_pump.c
    fb_present.c
)
target_include_directories(xbox_video PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)
target_link_libraries(xbox_video PUBLIC xbox_d3d8)
target_link_libraries(xbox_video PRIVATE xbox::ffmpeg)
