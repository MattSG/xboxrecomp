# MCPX APU Emulation Library
# Extracted from xemu (https://github.com/xemu-project/xemu)
#
# Voice Processor (VP): ADPCM/PCM decode, sample mixing, envelopes, filters
# Global/Encode Processors: DSP56300 interpreter + DMA

add_library(xbox_apu STATIC
    apu_core.c
    apu_vp.c
    apu_dsp.c
    dsp/dsp.c
    dsp/dsp_dma.c
    dsp/interp/dsp_cpu.c
    apu_mmio_hook.c
    apu_xaudio2.c
    apu_sdl2.c
    apu_wav.c
)

target_include_directories(xbox_apu PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# qemu_shim.h lives in nv2a/ - shared QEMU type abstraction
target_include_directories(xbox_apu PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../nv2a
)

if(MSVC)
    target_compile_options(xbox_apu PRIVATE
        /W3
        /wd4244  # conversion warnings (float/double/int)
        /wd4146  # unary minus on unsigned
        /wd4267  # size_t to int conversion
        # RelWithDebInfo's default /Ob1 keeps the DSP56300 interpreter's small
        # helpers out of line: a recorded GP frame took 400 us at /Ob1 and
        # 330 us at /Ob2, of a 667 us real-time budget.
        $<$<CONFIG:RelWithDebInfo>:/Ob2>
    )
    target_compile_definitions(xbox_apu PRIVATE
        _CRT_SECURE_NO_WARNINGS
        _USE_MATH_DEFINES
    )
endif()

# Recompiled DSP programs (tools/dsp_recomp): set XBOXRECOMP_DSP_PROGRAM_DIR to
# the folder the runtime saves the programs a title runs to
# (RECOMP_DSP_PROGRAM_DIR). Every build regenerates the C from whatever is
# there -- rewritten only when it changes -- and an empty folder leaves the
# interpreter on its own.
if(XBOXRECOMP_DSP_PROGRAM_DIR)
    set(dsp_interp ${CMAKE_CURRENT_SOURCE_DIR}/dsp/interp)
    set(dsp_recomp_inc ${CMAKE_CURRENT_BINARY_DIR}/dsp_recomp_programs.inc)
    add_executable(dsp_recomp_gen ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/dsp_recomp/dsp_recomp_gen.c)
    target_include_directories(dsp_recomp_gen PRIVATE ${dsp_interp} ${CMAKE_CURRENT_SOURCE_DIR}/dsp)
    target_compile_definitions(dsp_recomp_gen PRIVATE _CRT_SECURE_NO_WARNINGS NDEBUG)
    add_custom_target(dsp_recomp_programs
        COMMAND dsp_recomp_gen ${dsp_interp}/dsp_cpu.c ${dsp_interp}/dsp_emu.c.inc
                ${dsp_recomp_inc} ${XBOXRECOMP_DSP_PROGRAM_DIR}
        BYPRODUCTS ${dsp_recomp_inc}
        COMMENT "Recompiling DSP programs from ${XBOXRECOMP_DSP_PROGRAM_DIR}"
        VERBATIM)
    add_dependencies(xbox_apu dsp_recomp_programs)
    target_compile_definitions(xbox_apu PRIVATE DSP_RECOMP_INC="${dsp_recomp_inc}")
    set_source_files_properties(dsp/interp/dsp_cpu.c PROPERTIES OBJECT_DEPENDS ${dsp_recomp_inc})
endif()

if(WIN32)
    # xaudio2_8 for XAudio2Create, which apu_xaudio2.c calls directly.
    # It was missing: nothing had ever referenced this library from a linked
    # target, so the archive was built and never pulled in, and the first
    # target to actually call into the APU failed at link with an undefined
    # XAudio2Create rather than at build time here.
    target_link_libraries(xbox_apu PRIVATE platform kernel32 winmm xaudio2_8 ole32)
else()
    # POSIX (macOS, Linux): platform layer (Win32 primitives on POSIX),
    # SDL2 for the audio device (apu_sdl2.c) and libm
    find_package(SDL2 REQUIRED)
    target_link_libraries(xbox_apu PRIVATE platform SDL2::SDL2 m)
endif()
