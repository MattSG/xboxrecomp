# Xbox Input → XInput Compatibility Layer

add_library(xbox_input STATIC
    xinput_device.c
    keymap_spec.c
)

target_include_directories(xbox_input PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

if(WIN32)
    target_link_libraries(xbox_input PUBLIC platform xinput)
else()
    include(Sdl3)
    target_link_libraries(xbox_input PUBLIC platform SDL3::SDL3)
endif()

if(MSVC)
    target_compile_options(xbox_input PRIVATE /W4 /O2)
else()
    target_compile_options(xbox_input PRIVATE -Wall -Wextra -O2)
endif()
