cmake_minimum_required(VERSION 3.20)
project(input_sdl_pad C)
set(CMAKE_C_STANDARD 11)

# Windows only (the pad merge shares a translation unit with the Win32 raw-input mouse layer).
# The production SDL3 pad code is driven through an SDL virtual gamepad: no hardware needed.
if(WIN32)
    include(../../cmake/Sdl3.cmake)
    add_executable(input_sdl_pad test_main.c
        ../../src/input/keyboard.c ../../src/input/keymap_spec.c ../../src/input/input_map.c ../../src/input/mkb_input.c)
    target_include_directories(input_sdl_pad PRIVATE
        ../../src ../../src/input ../../src/video ../../src/kernel
        ../../src/platform ../../include ../../templates/runtime)
    target_compile_definitions(input_sdl_pad PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_link_libraries(input_sdl_pad PRIVATE SDL3::SDL3 user32)
    if(MSVC)
        target_compile_options(input_sdl_pad PRIVATE /UNDEBUG)
    endif()
    enable_testing()
    add_test(NAME input_sdl_pad COMMAND input_sdl_pad)
endif()
