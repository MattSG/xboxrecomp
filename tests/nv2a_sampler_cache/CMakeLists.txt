cmake_minimum_required(VERSION 3.20)
project(nv2a_sampler_cache C)
enable_testing()

add_executable(nv2a_sampler_cache test_sampler_cache.c)
target_include_directories(nv2a_sampler_cache PRIVATE ../../src/video)
add_test(NAME nv2a_sampler_cache_lifetime COMMAND nv2a_sampler_cache)
