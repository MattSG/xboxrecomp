# Xbox Input → XInput Compatibility Layer

add_library(xbox_input STATIC
    xinput_device.c
    keyboard.c          # the key table and the one key map (every host)
    keymap_spec.c       # RECOMP_KEYMAP's NAME:VK format
)

target_include_directories(xbox_input PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# The mapping rules (bindings, remapping, deadzones), also buildable alone;
# keyboard.c resolves its key map through them.
add_subdirectory(mapping)
target_link_libraries(xbox_input PUBLIC xbox_input_map)

if(WIN32)
    # Mouse+keyboard pad (RECOMP_MKB) reads Win32 raw input.
    target_sources(xbox_input PRIVATE mkb_input.c)
endif()

# One host pad path everywhere: SDL3 gamepads (Xbox, PlayStation and Switch
# pads, hot plugging, rumble). On Windows SDL reaches Xbox pads through XInput
# itself, so nothing here links xinput. With the SDL3 renderer the window
# (video/fb_window_sdl.c) pumps the events that keep their state current and
# records the keys; with Direct3D 11 xinput_device.c refreshes the pads itself.
include(Sdl3)
target_link_libraries(xbox_input PUBLIC platform SDL3::SDL3)

if(MSVC)
    target_compile_options(xbox_input PRIVATE /W4 /O2)
else()
    target_compile_options(xbox_input PRIVATE -Wall -Wextra -O2)
endif()
