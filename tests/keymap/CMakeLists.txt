cmake_minimum_required(VERSION 3.20)
project(keymap_regression LANGUAGES C)
add_executable(keymap-test keymap_test.c
    ../../src/input/keyboard.c ../../src/input/keymap_spec.c ../../src/input/input_map.c)
if(WIN32)
    target_sources(keymap-test PRIVATE ../../src/input/mkb_input.c)   # RECOMP_MKB
endif()
if(MSVC)
    target_compile_options(keymap-test PRIVATE /UNDEBUG)
else()
    target_compile_options(keymap-test PRIVATE -UNDEBUG)
endif()
target_include_directories(keymap-test PRIVATE ../../src)
if(NOT WIN32)
    # The pad is SDL3 here; the test never opens a gamepad, but the TU links SDL
    # and the POSIX Win32 primitives (_stricmp, GetTickCount64) it is built on.
    list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../../cmake")
    include(Sdl3)
    add_subdirectory(../../src/platform platform)
    target_link_libraries(keymap-test PRIVATE SDL3::SDL3 platform)
endif()
enable_testing()
add_test(NAME keymap COMMAND keymap-test)
add_test(NAME keymap-menu-click COMMAND keymap-test menu-click)
add_test(NAME keymap-default-padmap COMMAND keymap-test default-padmap)

if(NOT WIN32)
    add_test(NAME keymap-two-gamepads COMMAND keymap-test two-gamepads)
endif()
