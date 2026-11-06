# Builds the whole runtime with every bundled host backend left out
# (XBOXRECOMP_RENDERER=EXTERNAL, XBOXRECOMP_APU_OUTPUT=EXTERNAL) and links it
# against stub_backends.c. The link succeeds only while the stubs define
# exactly the functions the contracts name, so this is the checked list of
# what an embedding project's own renderer, window and audio device provide.
cmake_minimum_required(VERSION 3.24)
project(external_backends LANGUAGES C)

# -DBUNDLED_BACKENDS=ON links the same program against the bundled D3D11,
# window and audio backends instead: the default configuration's link check.
option(BUNDLED_BACKENDS "Link the bundled backends instead of the stubs" OFF)
if(NOT BUNDLED_BACKENDS)
    set(XBOXRECOMP_RENDERER EXTERNAL CACHE STRING "" FORCE)
    set(XBOXRECOMP_APU_OUTPUT EXTERNAL CACHE STRING "" FORCE)
endif()
set(XBOXRECOMP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../.. ${CMAKE_CURRENT_BINARY_DIR}/xboxrecomp)

add_executable(external_backends main.c)
if(BUNDLED_BACKENDS)
    target_sources(external_backends PRIVATE title_stubs.c)
else()
    target_sources(external_backends PRIVATE stub_backends.c)
endif()
target_link_libraries(external_backends PRIVATE xboxrecomp)
if(MSVC)
    # Pull every object in, so unresolved contract functions surface here.
    target_link_options(external_backends PRIVATE /WHOLEARCHIVE:xbox_video.lib /WHOLEARCHIVE:xbox_nv2a.lib /WHOLEARCHIVE:xbox_apu.lib)
endif()
xbox_copy_runtime_dlls(external_backends)
enable_testing()
add_test(NAME external_backends COMMAND external_backends)
