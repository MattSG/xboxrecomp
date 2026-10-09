cmake_minimum_required(VERSION 3.20)
project(apu_mixdown_test C)
enable_testing()
# DSP programs now own mixdown. Run the real execution/PCM regression checks.
add_subdirectory(../dsp dsp)
