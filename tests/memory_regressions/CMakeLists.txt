cmake_minimum_required(VERSION 3.20)
project(memory_regressions_test C CXX)
# Cross-platform, like tests/kernel_bridge: the heap and the memory bridge are
# the same code on both hosts.
set(CMAKE_C_STANDARD 11)
add_subdirectory(../.. runtime)
add_executable(memory_regressions_test test_main.c)
target_include_directories(memory_regressions_test PRIVATE ../../src/kernel)
target_link_libraries(memory_regressions_test PRIVATE xboxrecomp)
enable_testing()

set(XBE ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/conformance/test.xbe)
# One process per mode: the switches are read once and cached, so they cannot be
# flipped inside a run.
add_test(NAME memory_regressions_default COMMAND memory_regressions_test default ${XBE})
add_test(NAME memory_regressions_car_churn COMMAND memory_regressions_test car-churn ${XBE})
add_test(NAME memory_regressions_scene_churn COMMAND memory_regressions_test scene-churn ${XBE})
add_test(NAME memory_regressions_heap_reclaim COMMAND memory_regressions_test heap-reclaim ${XBE})
add_test(NAME memory_regressions_ext_vma COMMAND memory_regressions_test ext-vma ${XBE})
add_test(NAME memory_regressions_ext_vma_128 COMMAND memory_regressions_test ext-vma-128 ${XBE})
add_test(NAME memory_regressions_reserve_heap COMMAND memory_regressions_test reserve-heap ${XBE})

add_test(NAME memory_regressions_tls_layout COMMAND memory_regressions_test tls-layout ${XBE})
add_test(NAME memory_regressions_tls_bad_directory COMMAND memory_regressions_test tls-bad-directory ${XBE})
add_test(NAME memory_regressions_tls_bad_range COMMAND memory_regressions_test tls-bad-range ${XBE})
add_test(NAME memory_regressions_tls_size_overflow COMMAND memory_regressions_test tls-size-overflow ${XBE})
add_test(NAME memory_regressions_image_high COMMAND memory_regressions_test image-high ${XBE})
add_test(NAME memory_regressions_image_bad_range COMMAND memory_regressions_test image-bad-range ${XBE})
add_test(NAME memory_regressions_image_bad_raw COMMAND memory_regressions_test image-bad-raw ${XBE})
add_test(NAME memory_regressions_image_bad_table COMMAND memory_regressions_test image-bad-table ${XBE})
add_test(NAME memory_regressions_image_short_header COMMAND memory_regressions_test image-short-header ${XBE})

# Compile as a real C++ consumer: late thread APIs must keep the runtime C ABI.
add_executable(memory_cpp_linkage_test cpp_linkage.cpp)
target_include_directories(memory_cpp_linkage_test PRIVATE ../../src/kernel)
target_link_libraries(memory_cpp_linkage_test PRIVATE xboxrecomp)
add_test(NAME memory_cpp_linkage COMMAND memory_cpp_linkage_test)

add_test(NAME memory_regressions_stack_layout COMMAND memory_regressions_test stack-layout ${XBE})

foreach(case stack-config stack-bad-base stack-bad-size stack-unbacked)
    add_test(NAME memory_regressions_${case} COMMAND memory_regressions_test ${case} ${XBE})
endforeach()

add_executable(memory_thunk_table_test thunk_table.c)
target_include_directories(memory_thunk_table_test PRIVATE ../../src/kernel)
target_link_libraries(memory_thunk_table_test PRIVATE xboxrecomp)
foreach(case high-retail high-debug empty none encoded-none partial unterminated bad-ordinal bad-marker maximum sparse remapped remapped-data launch-page no-launch-page kernel-data-auto kernel-data-config kernel-data-overlap kernel-data-unbacked kernel-data-header kernel-data-empty kernel-identity kernel-identity-default kernel-strings kernel-strings-empty kernel-strings-invalid kernel-strings-max kernel-strings-oom thread-stack-many thread-stack-policy thread-stack-unbacked thread-stack-parallel stack-bounds kernel-stacks)
    add_test(NAME memory_thunk_${case} COMMAND memory_thunk_table_test ${case})
endforeach()
