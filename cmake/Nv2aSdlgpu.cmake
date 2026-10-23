# The SDL3 GPU renderer's shaders and dependencies, for any target that builds
# src/video/nv2a_sdlgpu.c (the runtime itself and the tests of that backend).
#
#   xbr_sdlgpu_shaders(<gen_dir> <out_var>)
#       Compiles each shaders/sdlgpu/ compile unit with DXC into
#       <gen_dir>/sdlgpu/<name>.<format>.h (SPIR-V; DXIL on Windows) and sets
#       <out_var> to the list of headers. nv2a_sdlgpu.c includes them.
#
#   xbr_nv2a_sdlgpu_target(<target>)
#       Gives <target> the generated headers, SDL3 (SDL3::SDL3) and the include
#       path nv2a_sdlgpu.c needs, with XBOXRECOMP_SDL3=1 defined, as the runtime
#       does. The caller's own source includes nv2a_sdlgpu.c and supplies the
#       host callbacks it leaves out (see tests/nv2a_sdlgpu_test_host.h).
#
# Uses the runtime's own rules (cmake/Sdl3.cmake, cmake/Dxc.cmake,
# cmake/Nv2aHlsl.cmake); nothing is copied here.
include_guard(GLOBAL)

get_filename_component(_xbr_sdlgpu_root "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
include(${_xbr_sdlgpu_root}/cmake/Nv2aHlsl.cmake)

function(xbr_sdlgpu_shaders gen out)
    set(_sdl_shaders
        "ub ub_vs vs"
        "combiner ps_main ps"
        "combiner ps_wdepth ps"
        "combiner ps_main ps ps_main_flat FLATQ=nointerpolation"
        "combiner ps_wdepth ps ps_wdepth_flat FLATQ=nointerpolation"
        "present present_vs vs"
        "present present_ps ps"
        "clear clear_vs vs"
        "clear clear_ps ps"
        "y16 y16_vs vs"
        "y16 y16_ps ps"
        "y16d y16d_ps ps")
    set(_formats spirv)
    if(WIN32)
        list(APPEND _formats dxil)
    endif()
    set(_shader_headers)
    foreach(_spec IN LISTS _sdl_shaders)
        string(REPLACE " " ";" _spec "${_spec}")
        list(GET _spec 0 _unit)
        list(GET _spec 1 _entry)
        list(GET _spec 2 _stage)
        set(_name ${_entry})
        set(_defines)
        list(LENGTH _spec _n)
        if(_n GREATER 3)
            list(GET _spec 3 _name)
            list(GET _spec 4 _define)
            set(_defines -D ${_define})
        endif()
        # Resolve the same selected fragments as D3D11's embedded source.
        set(_unit_input "${NV2A_HLSL_DIR}/sdlgpu/${_unit}.hlsl")
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_unit_input}")
        file(READ "${_unit_input}" _unit_text)
        foreach(_part IN LISTS NV2A_HLSL_PARTS)
            get_filename_component(_part_name "${_part}" NAME)
            string(REPLACE "\"../${_part_name}\"" "\"${_part}\"" _unit_text "${_unit_text}")
        endforeach()
        set(_unit_source "${gen}/units/${_unit}.hlsl")
        file(CONFIGURE OUTPUT "${_unit_source}" CONTENT "${_unit_text}" @ONLY)
        string(TOUPPER ${_stage} _STAGE)
        foreach(_format IN LISTS _formats)
            set(_out ${gen}/sdlgpu/${_name}.${_format}.h)
            if(_format STREQUAL "spirv")
                set(_flags -spirv -fspv-target-env=vulkan1.0 -fvk-use-dx-layout)
            else()
                set(_flags)
            endif()
            add_custom_command(OUTPUT ${_out}
                COMMAND ${CMAKE_COMMAND} -E make_directory ${gen}/sdlgpu
                COMMAND ${XBOXRECOMP_DXC} -nologo -HV 2018 -Wno-ignored-attributes
                        -T ${_stage}_6_0 -E ${_entry} -D SDLGPU -D STAGE_${_STAGE} ${_defines}
                        ${_flags} -Fh ${_out} -Vn ${_name}_${_format}
                        ${_unit_source}
                DEPENDS ${_unit_source} ${_unit_input} ${NV2A_HLSL_DEPENDS}
                COMMENT "dxc ${_name} (${_format})"
                VERBATIM)
            list(APPEND _shader_headers ${_out})
        endforeach()
    endforeach()
    set(${out} ${_shader_headers} PARENT_SCOPE)
endfunction()

function(xbr_nv2a_sdlgpu_target target)
    include(${_xbr_sdlgpu_root}/cmake/Dxc.cmake)
    include(${_xbr_sdlgpu_root}/cmake/Sdl3.cmake)
    set(_gen ${CMAKE_CURRENT_BINARY_DIR}/${target}_generated)
    xbr_sdlgpu_shaders(${_gen} _headers)
    target_sources(${target} PRIVATE ${_headers})
    target_include_directories(${target} PRIVATE ${_gen}
        ${_xbr_sdlgpu_root}/src ${_xbr_sdlgpu_root}/src/video)
    target_link_libraries(${target} PRIVATE SDL3::SDL3)
    target_compile_definitions(${target} PRIVATE XBOXRECOMP_SDL3=1)
endfunction()
