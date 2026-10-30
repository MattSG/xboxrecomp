cmake_minimum_required(VERSION 3.20)
project(input_xinput_imports C)
# A Windows exe linked against xbox_input must import no XInput DLL: the pads go
# through SDL3, which loads XInput itself at run time. (A static libxinput.a from
# mingw-w64 would name XINPUT1_3.dll, the DirectX redistributable that stock
# Windows 10/11 lacks, and the exe would fail to start with 0xC0000135.) The
# probe links xbox_input exactly as a game does and the check reads its import
# table on the build host with objdump, so no Windows or emulator is needed.
# llvm-mingw and mingw-w64 provide CMAKE_OBJDUMP; MSVC does not.
if(NOT WIN32)
    message(FATAL_ERROR "This check reads a Windows exe's import table.")
endif()
if(NOT CMAKE_OBJDUMP)
    message(FATAL_ERROR "This check needs an objdump (a mingw toolchain).")
endif()
set(CMAKE_C_STANDARD 11)
# Only xbox_input and what it links are built.
add_subdirectory(../.. runtime EXCLUDE_FROM_ALL)
add_executable(input_xinput_probe probe.c)
target_link_libraries(input_xinput_probe PRIVATE xbox_input)
enable_testing()
add_test(NAME input_xinput_imports
         COMMAND ${CMAKE_COMMAND}
                 -DOBJDUMP=${CMAKE_OBJDUMP}
                 -DEXE=$<TARGET_FILE:input_xinput_probe>
                 -P ${CMAKE_CURRENT_SOURCE_DIR}/check_imports.cmake)
