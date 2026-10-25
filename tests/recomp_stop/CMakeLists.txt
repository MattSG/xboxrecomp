cmake_minimum_required(VERSION 3.20)
project(recomp_stop_test C CXX)

set(CMAKE_C_STANDARD 11)
add_subdirectory(../.. runtime)
add_executable(recomp_stop_test test_main.c)
target_include_directories(recomp_stop_test PRIVATE ../../templates/runtime)
target_link_libraries(recomp_stop_test PRIVATE xboxrecomp)
enable_testing()

if(MSVC)
    add_test(NAME recomp_stop COMMAND recomp_stop_test)
endif()
