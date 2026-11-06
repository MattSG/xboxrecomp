cmake_minimum_required(VERSION 3.20)
project(media_decode_test C)
set(CMAKE_C_STANDARD 11)
# Standalone, or part of a combined tree that already has the runtime.
if(NOT TARGET xboxrecomp)
    add_subdirectory(../.. runtime)
endif()
add_executable(media_decode_test test_main.c)
target_link_libraries(media_decode_test PRIVATE xbox_video xbox_dsound)
if(XBOX_FFMPEG_ROOT)
    # The test makes its own media with the FFmpeg that was downloaded.
    target_compile_definitions(media_decode_test PRIVATE
        XBOX_FFMPEG_BIN="${XBOX_FFMPEG_ROOT}/bin/ffmpeg.exe")
endif()
xbox_copy_runtime_dlls(media_decode_test)
enable_testing()
add_test(NAME media_decode_test COMMAND media_decode_test)
