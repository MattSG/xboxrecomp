# FFmpeg for media decoding (FMV video, WMA audio). SDL3 has no decoder.
#
# Provides the imported target xbox::ffmpeg (libavformat, libavcodec, libavutil,
# libswscale, libswresample), and xbox_copy_ffmpeg(<target>) for hosts where the
# libraries are shared libraries that must sit beside the executable.
#
# Linux/macOS: the system FFmpeg (pkg-config). Windows: a pinned LGPL shared
# build from BtbN/FFmpeg-Builds, downloaded and verified by hash. The shared
# libraries are LGPL-3.0 and are loaded dynamically; see NOTICE.
if(NOT TARGET xbox::ffmpeg)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(XBOX_FFMPEG_PC QUIET IMPORTED_TARGET GLOBAL
            libavformat libavcodec libavutil libswscale libswresample)
    endif()

    if(XBOX_FFMPEG_PC_FOUND)
        add_library(xbox_ffmpeg INTERFACE)
        target_link_libraries(xbox_ffmpeg INTERFACE PkgConfig::XBOX_FFMPEG_PC)
        add_library(xbox::ffmpeg ALIAS xbox_ffmpeg)
    elseif(WIN32)
        include(FetchContent)
        FetchContent_Declare(ffmpeg_win
            URL https://github.com/BtbN/FFmpeg-Builds/releases/download/autobuild-2026-10-30-13-01/ffmpeg-n8.1.3-27-g8dca1586f4-win64-lgpl-shared-8.1.zip
            URL_HASH SHA256=21b96a5b382b031e89c1c91d7af8efc3db989c4c11bff27422a4c4ea24cf45ed)
        FetchContent_MakeAvailable(ffmpeg_win)
        set(XBOX_FFMPEG_ROOT "${ffmpeg_win_SOURCE_DIR}" CACHE INTERNAL "")
        add_library(xbox_ffmpeg INTERFACE)
        target_include_directories(xbox_ffmpeg INTERFACE "${XBOX_FFMPEG_ROOT}/include")
        target_link_libraries(xbox_ffmpeg INTERFACE
            "${XBOX_FFMPEG_ROOT}/lib/avformat.lib" "${XBOX_FFMPEG_ROOT}/lib/avcodec.lib"
            "${XBOX_FFMPEG_ROOT}/lib/avutil.lib" "${XBOX_FFMPEG_ROOT}/lib/swscale.lib"
            "${XBOX_FFMPEG_ROOT}/lib/swresample.lib")
        add_library(xbox::ffmpeg ALIAS xbox_ffmpeg)
    else()
        message(FATAL_ERROR "FFmpeg development files not found (install libavformat/libavcodec/"
                            "libavutil/libswscale/libswresample with headers, e.g. tools/linux/install_deps.sh)")
    endif()
endif()

# Copy the FFmpeg DLLs next to an executable (Windows download only).
function(xbox_copy_ffmpeg target)
    if(WIN32 AND XBOX_FFMPEG_ROOT)
        file(GLOB _ff_dlls "${XBOX_FFMPEG_ROOT}/bin/*.dll")
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${_ff_dlls} $<TARGET_FILE_DIR:${target}>)
    endif()
endfunction()

# Every executable that links the runtime libraries needs the DLLs beside it
# on Windows: SDL_shadercross's DXC (dxcompiler.dll, dxil.dll) and FFmpeg are
# imported by the static libraries whether or not a particular program uses
# them. Call this once per executable.
function(xbox_copy_runtime_dlls target)
    xbox_copy_dxc(${target})
    xbox_copy_ffmpeg(${target})
endfunction()
