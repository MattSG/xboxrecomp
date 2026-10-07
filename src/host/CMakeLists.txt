# Host bring-up a title's main() otherwise copies: the emulated APU, USB and
# NV2A wired to their MMIO traps, and the window and D3D device.

add_library(xbox_host STATIC host.c)

target_include_directories(xbox_host PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(xbox_host PUBLIC platform xbox_kernel xbox_apu xbox_usb xbox_nv2a xbox_d3d8)

if(MSVC)
    target_compile_options(xbox_host PRIVATE /W4 /O2)
else()
    target_compile_options(xbox_host PRIVATE -Wall -Wextra -O2)
endif()
