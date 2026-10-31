/**
 * Run-time HLSL -> SDL_GPU shader compilation (DXC + SPIRV-Cross through
 * SDL_shadercross). The Xbox shaders are generated as text while the game
 * runs, so this cannot be a build step.
 *
 * Resource spaces follow SDL_GPU's SPIR-V convention, and the HLSL must
 * declare them: vertex uniform buffers register(bN, space1); fragment
 * textures/samplers register(tN/sN, space2) and uniform buffers
 * register(bN, space3). Vertex textures use space0.
 */
#ifndef XBOX_GPU_SHADER_H
#define XBOX_GPU_SHADER_H

#include <SDL3/SDL_gpu.h>

/* Returns NULL on failure (the error is in SDL_GetError / stderr). */
SDL_GPUShader *xgpu_compile_hlsl(SDL_GPUDevice *device, const char *hlsl,
                                 const char *entry, int is_fragment,
                                 const char *name);

#endif
