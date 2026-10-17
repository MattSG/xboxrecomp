/* Resource bindings, for both host renderers.
 *
 * D3D11 (fxc, shader model 5, nv2a_d3d11.c): the plain registers.
 *
 * SDL3 GPU (dxc, shader model 6, nv2a_sdlgpu.c; built with SDLGPU and
 * STAGE_VS or STAGE_PS defined): SDL's fixed layout. A vertex shader's
 * storage buffers are t#, space0 and its uniform buffers b#, space1; a
 * fragment shader's sampled textures are t#, space2, each paired with the
 * sampler s# of the same number (combined image samplers in SPIR-V), and its
 * uniform buffers b#, space3. A texture sampled with two different samplers
 * therefore takes two slots (the *_P / *_L / SC# names below; on D3D11 they
 * are the one texture and its sampler).
 *
 * RT_SAMPLES: the render target's sample count. SDL3 GPU targets are
 * single-sample, and SPIR-V has no query for it. */
#ifdef SDLGPU
#  ifdef STAGE_VS
#    define CBUF(n) register(b##n, space1)
#  else
#    define CBUF(n) register(b##n, space3)
#  endif
#  define VS_BUFFER(n) register(t##n, space0)
#  define PS_TEX(type, name, n) [[vk::combinedImageSampler]] type name : register(t##n, space2)
#  define PS_SAMP(name, n) [[vk::combinedImageSampler]] SamplerState name : register(s##n, space2)
#  define RT_SAMPLES 1u
#else
#  define CBUF(n) register(b##n)
#  define VS_BUFFER(n) register(t##n)
#  define PS_TEX(type, name, n) type name : register(t##n)
#  define PS_SAMP(name, n) SamplerState name : register(s##n)
#  define RT_SAMPLES GetRenderTargetSampleCount()
#endif
