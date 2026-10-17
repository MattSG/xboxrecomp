# The NV2A renderer's HLSL (src/video/shaders): the parts in the order the
# D3D11 renderer compiles them, and xbr_embed_nv2a_hlsl(<target>), which gives
# a target that compiles src/video/nv2a_d3d11.c the generated nv2a_hlsl.inc it
# includes. Used by src/video and by the tests that build the renderer
# directly.
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
set(_XBR_EMBED_TEXT ${_xbr_hlsl_root}/cmake/EmbedText.cmake)

function(xbr_embed_nv2a_hlsl target)
    set(gen ${CMAKE_CURRENT_BINARY_DIR}/${target}_generated)
    string(JOIN "|" parts ${NV2A_HLSL_PARTS})
    add_custom_command(OUTPUT ${gen}/nv2a_hlsl.inc
        COMMAND ${CMAKE_COMMAND} -DNAME=s_hlsl -DOUT=${gen}/nv2a_hlsl.inc
                "-DINPUTS=${parts}" -P ${_XBR_EMBED_TEXT}
        DEPENDS ${NV2A_HLSL_PARTS} ${_XBR_EMBED_TEXT}
        COMMENT "Embedding the D3D11 renderer's HLSL for ${target}"
        VERBATIM)
    target_sources(${target} PRIVATE ${gen}/nv2a_hlsl.inc)
    target_include_directories(${target} PRIVATE ${gen})
endfunction()
