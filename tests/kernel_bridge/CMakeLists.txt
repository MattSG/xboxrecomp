cmake_minimum_required(VERSION 3.20)
project(kernel_bridge_test C)
# Cross-platform, unlike the _posix projects beside it: nothing here is
# POSIX-specific, and the bridge is the same code on both hosts. The one thing
# that differs is xbox_kernel_bridge_init()'s VirtualProtect over the thunk
# region -- real on Windows, shimmed elsewhere -- which is exactly why it is
# worth running in both places.
set(CMAKE_C_STANDARD 11)
add_subdirectory(../.. runtime)
add_executable(kernel_bridge_test test_main.c)
target_include_directories(kernel_bridge_test PRIVATE ../../src/kernel)
target_link_libraries(kernel_bridge_test PRIVATE xboxrecomp)
enable_testing()
add_test(NAME kernel_bridge_test COMMAND kernel_bridge_test)
set_tests_properties(kernel_bridge_test PROPERTIES TIMEOUT 20)

foreach(mode hook default clear)
    add_test(NAME kernel_firmware_${mode} COMMAND kernel_bridge_test firmware-${mode})
    set_tests_properties(kernel_firmware_${mode} PROPERTIES TIMEOUT 5)
endforeach()
set_tests_properties(kernel_firmware_hook PROPERTIES PASS_REGULAR_EXPRESSION "FIRMWARE_HOOK=37" FAIL_REGULAR_EXPRESSION "FIRMWARE_RETURNED")
set_tests_properties(kernel_firmware_default kernel_firmware_clear PROPERTIES
    PASS_REGULAR_EXPRESSION "HalReturnToFirmware routine=37" FAIL_REGULAR_EXPRESSION "FIRMWARE_HOOK;FIRMWARE_RETURNED")
