# The NV2A renderer's HLSL (src/video/shaders): the parts in the order the
# D3D11 renderer compiles them, and xbr_embed_nv2a_hlsl(<target>), which gives
# a target that compiles src/video/nv2a_d3d11.c the generated nv2a_hlsl.inc it
# includes. Used by src/video and by the tests that build the renderer
# directly. Port selection: docs/technical/shader-overrides.md.
get_filename_component(_xbr_hlsl_root "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(NV2A_HLSL_DIR ${_xbr_hlsl_root}/src/video/shaders)
set(NV2A_HLSL_PARTS
    ${NV2A_HLSL_DIR}/nv2a_bindings.hlsli
    ${NV2A_HLSL_DIR}/nv2a_varyings.hlsli
    ${NV2A_HLSL_DIR}/nv2a_vertex.hlsl
    ${NV2A_HLSL_DIR}/nv2a_combiner.hlsl
    ${NV2A_HLSL_DIR}/nv2a_present.hlsl
    ${NV2A_HLSL_DIR}/nv2a_clear.hlsl
    ${NV2A_HLSL_DIR}/nv2a_y16.hlsl
    ${NV2A_HLSL_DIR}/nv2a_y16d.hlsl)
# Optional application-owned replacements, by the shipped fragment filename.
# Select before adding the toolkit. Unspecified fragments retain shared sources.
if(XBOXRECOMP_NV2A_SHADER_DIR)
    if(NOT IS_ABSOLUTE "${XBOXRECOMP_NV2A_SHADER_DIR}" OR
       NOT IS_DIRECTORY "${XBOXRECOMP_NV2A_SHADER_DIR}")
        message(FATAL_ERROR "XBOXRECOMP_NV2A_SHADER_DIR must be an existing absolute directory")
    endif()
    file(GLOB_RECURSE _xbr_shader_dependencies CONFIGURE_DEPENDS
        "${XBOXRECOMP_NV2A_SHADER_DIR}/*.hlsl" "${XBOXRECOMP_NV2A_SHADER_DIR}/*.hlsli")
    set(_xbr_selected_parts)
    foreach(_part IN LISTS NV2A_HLSL_PARTS)
        get_filename_component(_name "${_part}" NAME)
        if(EXISTS "${XBOXRECOMP_NV2A_SHADER_DIR}/${_name}")
            list(APPEND _xbr_selected_parts "${XBOXRECOMP_NV2A_SHADER_DIR}/${_name}")
        else()
            list(APPEND _xbr_selected_parts "${_part}")
        endif()
    endforeach()
    set(NV2A_HLSL_PARTS ${_xbr_selected_parts})
else()
    set(_xbr_shader_dependencies)
endif()
set(NV2A_HLSL_DEPENDS ${NV2A_HLSL_PARTS} ${_xbr_shader_dependencies})

set(_XBR_EMBED_TEXT ${_xbr_hlsl_root}/cmake/EmbedText.cmake)

function(xbr_embed_nv2a_hlsl target)
    set(gen ${CMAKE_CURRENT_BINARY_DIR}/${target}_generated)
    string(JOIN "|" parts ${NV2A_HLSL_PARTS})
    add_custom_command(OUTPUT ${gen}/nv2a_hlsl.inc
        COMMAND ${CMAKE_COMMAND} -DNAME=s_hlsl -DOUT=${gen}/nv2a_hlsl.inc
                "-DINPUTS=${parts}" -P ${_XBR_EMBED_TEXT}
        DEPENDS ${NV2A_HLSL_DEPENDS} ${_XBR_EMBED_TEXT}
        COMMENT "Embedding the D3D11 renderer's HLSL for ${target}"
        VERBATIM)
    target_sources(${target} PRIVATE ${gen}/nv2a_hlsl.inc)
    target_include_directories(${target} PRIVATE ${gen})
endfunction()
