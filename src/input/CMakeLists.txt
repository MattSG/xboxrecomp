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

# POSIX only: the bundled SDL2 window feeds the key table through keyboard_sdl.c.
# An embedding project with its own window (an SDL3 host) turns this OFF so SDL2
# is not linked beside its SDL3.
option(XBOXRECOMP_INPUT_SDL2 "Link SDL2 and feed keys from keyboard_sdl.c (POSIX)" ON)
if(WIN32)
    # Mouse+keyboard pad (RECOMP_MKB) reads Win32 raw input.
    target_sources(xbox_input PRIVATE mkb_input.c)
    # mingw-w64's libxinput.a imports XINPUT1_3.dll, the DirectX June 2010
    # redistributable: stock Windows 10/11 lacks it and the exe dies with
    # 0xC0000135 before main. Proton ships it, so only native Windows shows
    # this. XINPUT1_4.dll is part of Windows since 8. The Windows SDK's
    # xinput.lib already imports 1_4 and the SDK has no xinput1_4.lib, so
    # MSVC keeps the plain name. tests/input_xinput_imports checks the result.
    target_link_libraries(xbox_input PUBLIC platform
        $<IF:$<BOOL:${MINGW}>,xinput1_4,xinput>)
elseif(NOT XBOXRECOMP_INPUT_SDL2)
    # XBOXRECOMP_INPUT_SDL2=OFF: the embedding project owns the window and feeds this table from its own
    # events (an SDL3 host maps SDL3 keycodes itself), so nothing here may pull
    # in SDL2: its symbols clash with SDL3's in one process.
    target_link_libraries(xbox_input PUBLIC platform)
else()
    find_package(SDL2 REQUIRED)
    # The SDL window's key and focus events into that table.
    target_sources(xbox_input PRIVATE keyboard_sdl.c)
    target_link_libraries(xbox_input PUBLIC platform SDL2::SDL2)
endif()

if(MSVC)
    target_compile_options(xbox_input PRIVATE /W4 /O2)
else()
    target_compile_options(xbox_input PRIVATE -Wall -Wextra -O2)
endif()
