cmake_minimum_required(VERSION 3.20)
project(input_capture_contract C)

# Windows only: the production pad merge code (its host pad is a fixture here) and user32.
if(WIN32)
    add_executable(input_capture_contract input_capture_contract.c
        ../../src/input/keyboard.c ../../src/input/keymap_spec.c ../../src/input/input_map.c ../../src/input/mkb_input.c)
    target_include_directories(input_capture_contract PRIVATE
        ../../src ../../src/input ../../src/video ../../src/kernel
        ../../src/platform ../../include ../../templates/runtime)
    target_link_libraries(input_capture_contract PRIVATE user32)
    if(MSVC)
        target_compile_options(input_capture_contract PRIVATE /UNDEBUG)
    endif()
    enable_testing()
    add_test(NAME input_capture COMMAND input_capture_contract)
endif()
