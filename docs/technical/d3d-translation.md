# D3D8 on SDL_GPU

How the Xbox's modified Direct3D 8 API is bridged to SDL3's GPU API (SDL_GPU) for rendering recompiled game code. SDL_GPU runs on Direct3D 12, Vulkan and Metal, so one code path covers Windows, Linux and macOS.

## Overview

The original Xbox uses a custom version of Direct3D 8, modified by Microsoft for the NV2A GPU. It is not identical to PC D3D8 -- it includes Xbox-specific extensions for push buffers, tiled memory, and GPU-specific features. The recompiled game code calls D3D8 methods through COM vtable pointers, and our translation layer intercepts these calls and routes them to SDL_GPU.

## Architecture

```
  Recompiled Game Code
         |
   COM vtable dispatch (RECOMP_ICALL)
         |
   D3D8 Compatibility Layer (src/d3d/d3d8_*.c)
         |
   ┌─────┴──────────────┐
   │ d3d8_device.c      │  Device state, render states, transforms, draw entry points
   │ d3d8_resources.c   │  Vertex/index buffers, textures, surfaces
   │ d3d8_shaders.c     │  Fixed-function pipeline → HLSL shaders
   │ d3d8_combiners.c   │  NV2A register combiners → HLSL pixel shaders
   │ d3d8_vsh.c         │  NV2A vertex programs → HLSL vertex shaders
   │ d3d8_states.c      │  Render states → pipeline key; sampler cache
   │ d3d8_pipeline.c    │  Pipeline cache, uniforms, bindings, draw
   └─────┬──────────────┘
         |
   src/gfx: gpu_device.c (window, device, passes, uploads)
            gpu_shader.c (HLSL → DXC → SPIR-V → DXIL / MSL / SPIR-V)
         |
   SDL_GPU  →  Direct3D 12 | Vulkan | Metal
```

## COM Vtable Emulation

Xbox D3D8 uses COM interfaces. Game code calls methods through vtable pointers:

```asm
; eax = IDirect3DDevice8 pointer
mov ecx, [eax]          ; ecx = vtable pointer
call [ecx + 0x40]       ; call method at vtable offset 0x40
```

The compatibility layer provides a struct with function pointers matching the IDirect3DDevice8 vtable layout (`d3d8_xbox.h`), so recompiled indirect calls land on the C implementations directly.

## Frames, passes and the backbuffer

SDL_GPU records into a command buffer and does not allow copy passes inside render passes, but D3D8 freely interleaves uploads (`Unlock`, `UpdateTexture`-style writes) with draws. `gpu_device.c` therefore owns pass state: `xgpu_copy_pass()` and `xgpu_render_pass()` end the other kind first, a render pass is reused while its targets stay the same, and a `Clear` starts a new pass with clear load operations (separate flags for colour, depth and stencil).

The D3D8 backbuffer is an offscreen texture owned by the device, not the swapchain image. Present acquires the swapchain, draws the backbuffer into it (a plain blit, or the gamma pass when a gamma ramp is set) and submits. Because of that, `GetBackBuffer`, `LockRect` and `CopyRects`-style readbacks work mid-frame: a readback submits the command buffer, waits, and the frame continues in a new one.

## Resources

Buffers and textures keep a system-memory copy that `Lock` hands out.

- **Vertex/index buffers** mark themselves dirty on `Unlock`; `d3d8_vb_sync` / `d3d8_ib_sync` upload them at the next draw that uses them.
- **Textures** (2D, cube, volume) unswizzle and convert to the GPU format on `UnlockRect` / `UnlockBox`, then upload through a per-frame transfer ring (`xgpu_stage`).
- **Surfaces** wrap a GPU texture plus mip level and array layer, and hold a reference to it (`XGpuTex`, since SDL_GPU textures have no refcount of their own). `LockRect` reads the region back into a host buffer and writes it back on unlock unless the lock was read-only.
- **`DrawPrimitiveUP`** data goes through a streaming ring buffer; triangle fans and quad lists are converted to triangle lists.

## Pipelines

D3D11 let a layer set shaders, input layout and state objects one at a time. SDL_GPU bakes shaders, vertex layout, blend, depth/stencil, raster state and target formats into one *graphics pipeline*. The other modules therefore only describe what is bound:

- `d3d8_shaders.c` / `d3d8_combiners.c` / `d3d8_vsh.c` choose the vertex and pixel shader, give the vertex layout, and write uniform data (`d3d8_pipe_set_vs`, `_set_ps`, `_set_layout`, `_set_uniforms`).
- `d3d8_states.c` turns the D3D8 render states into a plain, memcmp-comparable `D3D8StateKey` and caches samplers.
- `d3d8_pipe_draw` combines those with the target formats into a key, finds or creates the pipeline, binds uniforms (every uniform buffer a shader declares must be pushed), samplers and buffers, and draws.

### State translation

| D3D8 | SDL_GPU |
|------|---------|
| `ALPHABLENDENABLE`, `SRCBLEND`, `DESTBLEND`, `BLENDOP`, `COLORWRITEENABLE` | blend state of the colour target (colour factors are replaced by their alpha components in the alpha channel) |
| `ZENABLE`, `ZWRITEENABLE`, `ZFUNC`, stencil states | depth/stencil state (stencil reference is set per draw) |
| `CULLMODE`, `FILLMODE` | rasteriser state (point fill falls back to wireframe) |
| `D3DTSS_*FILTER`, `ADDRESSU/V` | cached `SDL_GPUSampler` (border colour and mirror-once addressing are approximated) |
| viewport | `SDL_SetGPUViewport` at draw time |

## Shaders

D3D11 compiled HLSL with `D3DCompile`. SDL_GPU takes SPIR-V, DXIL or MSL, so the generated HLSL goes through `gpu_shader.c`: DXC compiles it to SPIR-V, SDL_shadercross reflects it and cross-compiles to the backend's format. This happens when a shader is first needed, because the pixel combiners and vertex programs depend on state only known at run time.

The generators follow SDL_GPU's resource conventions, which the HLSL must spell out:

- vertex uniform buffers: `register(bN, space1)`; fragment textures and samplers: `register(tN/sN, space2)`; fragment uniform buffers: `register(bN, space3)`;
- vertex inputs and the vertex-to-pixel varyings carry explicit `[[vk::location(N)]]` (SPIR-V matches by number, not semantic): position 0, normal 1, diffuse 2, specular 3, texcoords 4–7 on input; diffuse 0, specular 1, texcoords 2–5, fog 6 on the varyings;
- sampler and uniform bindings must be contiguous from 0, so shaders that declare textures always declare and use all four stages (unused stages get a white texture bound).

DXC loads `dxcompiler.dll` / `dxil.dll` at run time; executables call `xbox_copy_dxc(<target>)` (cmake/xbox_sdl3.cmake) so the vendored copies sit beside them. A system `dxcompiler.dll` without SPIR-V support will not work.

### Fixed-function pipeline

D3D8's fixed-function pipeline is configured through render states and texture stage states. SDL_GPU has none, so `d3d8_shaders.c` emits a vertex shader (transform, up to 8 lights, vertex fog, per-stage texture coordinate generation) and a pixel shader per texture-type signature (2D / cube / 3D per stage) that interprets `D3DTSS_COLOROP` and friends from a constant buffer.

### NV2A register combiners and vertex programs

Xbox titles that use pixel shaders set NV2A combiner state; `d3d8_combiners.c` generates an HLSL pixel shader for each distinct combiner configuration (cached, LRU). `d3d8_vsh.c` translates NV2A vertex-program microcode to HLSL, with constants in a 192-register uniform buffer.

## Vertex formats

FVF flags become SDL_GPU vertex attributes in one vertex buffer slot. Components missing from an FVF get a dummy attribute at offset 0 that the shader ignores via its flags constant.

## Texture formats

See [src/d3d/README.md](../../src/d3d/README.md) for the full table. Swizzled formats are unswizzled to linear at upload; formats with no SDL_GPU equivalent are converted in software (YUV → BGRA, P8 → palette-expanded BGRA, AL8 → 16-bit, bit-reordered 16-bit, `X8R8G8B8` forced opaque because SDL_GPU has no BGRX format).

## Known limits

- Multisampled render targets are created single-sampled.
- `LockRect` on depth or block-compressed surfaces is not supported.
- D3D8 border-colour and mirror-once addressing are approximated.
- Clear rectangles are not honoured; the whole attachment is cleared.
- Verified with offscreen tests on Direct3D 12 (`tests/gpu_smoke`, `gpu_pass`, `gpu_xfer`, `d3d8_gpu_draw`, `d3d8_smoke`). Vulkan and Metal use the same code path but have not been exercised.
