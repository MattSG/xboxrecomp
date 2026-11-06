cmake_minimum_required(VERSION 3.20)
project(input_usb_report C)

# Windows only: the production pad merge (its host pad is a fixture here) and the
# USB report encoder, with the keyboard and mouse sources the merge needs.
if(WIN32)
    add_executable(input_usb_report test_main.c
        ../../src/input/keyboard.c ../../src/input/keymap_spec.c ../../src/input/input_map.c ../../src/input/mkb_input.c)
    target_include_directories(input_usb_report PRIVATE
        ../../src ../../src/input ../../src/usb ../../src/video ../../src/kernel
        ../../src/platform ../../include ../../templates/runtime)
    target_compile_definitions(input_usb_report PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_link_libraries(input_usb_report PRIVATE user32)
    if(MSVC)
        target_compile_options(input_usb_report PRIVATE /UNDEBUG)
    endif()
    enable_testing()
    add_test(NAME input_usb_report COMMAND input_usb_report)
endif()
