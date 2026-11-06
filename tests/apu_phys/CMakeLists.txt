cmake_minimum_required(VERSION 3.20)
project(apu_phys_test C)
if(NOT WIN32)
    add_subdirectory(../../src/platform platform)
endif()
enable_testing()
add_executable(apu_phys_test apu_phys_test.c)
target_include_directories(apu_phys_test PRIVATE ../../src ../../src/apu)
target_compile_definitions(apu_phys_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS _USE_MATH_DEFINES)
add_test(NAME apu_phys COMMAND apu_phys_test)
# apu_core.c is compiled whole; its other entry points (VP, DSP, xaudio2, ISR
# and register globals) are stubbed in apu_phys_test.c, each aborting if called.

if(NOT WIN32)
    target_link_libraries(apu_phys_test PRIVATE platform m)
endif()
