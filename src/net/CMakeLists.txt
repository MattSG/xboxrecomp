# Network: the MCPX Ethernet controller a title's XNet drives, and the NAT and
# peer tunnel that carry its frames (RECOMP_NET)

add_library(xbox_net STATIC
    nvnet.c
    net_backend.c
)

target_include_directories(xbox_net PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(xbox_net PUBLIC platform)
if(WIN32)
    target_link_libraries(xbox_net PUBLIC ws2_32 iphlpapi winmm)
endif()

if(MSVC)
    target_compile_options(xbox_net PRIVATE /W4 /O2)
else()
    target_compile_options(xbox_net PRIVATE -Wall -Wextra -O2)
endif()
