# xbox_d3d8 on SDL GPU

This layer implements the Xbox Direct3D 8 interface using SDL3 GPU. It translates
D3D8 state into cached graphics pipelines and generates HLSL for fixed-function
rendering, NV2A register combiners and vertex programs. SDL_shadercross/DXC
compile shaders for the selected host backend.

This is distinct from the native NV2A pushbuffer executor used by titles that
retain their statically linked XDK. Test both paths when a port uses both.

## Integration

Link `xbox_d3d8`, include `d3d8_xbox.h`, and create a device with
`xbox_Direct3DCreate8`. Device creation owns the SDL window and GPU device;
`hDeviceWindow` is ignored. `d3d8_PresentFrame()` presents the offscreen
backbuffer and pumps events. Set `RECOMP_HIDE_WINDOW=1` for hidden fixtures.

Call `xbox_copy_runtime_dlls(your_executable)` in the consuming CMake project
to copy required Windows DXC and FFmpeg libraries beside the executable.

## Implementation

| Source | Responsibility |
|---|---|
| `d3d8_device.c` | Device, state, draw entry points and presentation |
| `d3d8_pipeline.c` | Pipeline cache, bindings, uniforms and draw dispatch |
| `d3d8_resources.c` | Textures, surfaces, buffers and format conversion |
| `d3d8_shaders.c` | Fixed-function shaders |
| `d3d8_combiners.c`, `d3d8_vsh.c` | Xbox combiner and vertex-program translation |
| `d3d8_states.c` | Pipeline keys and samplers |
| `d3d8_clear.c`, `d3d8_gamma.c` | Clear operations and display gamma |
| `../gfx/gpu_device.c` | SDL GPU device, pass management and uploads |
| `../gfx/gpu_shader.c` | Shader compilation |
| `../gfx/sdl_events.c` | D3D8 window events and shared keyboard state |

Resources retain host copies for locking. Uploads and render passes are ordered
through the GPU helper; gamma applies at presentation without modifying guest
render targets. See the public header for the interface and the source for
per-format behavior.

## Limits and checks

D3D8 multisampled targets are currently created single-sampled. Border-colour
and mirror-once addressing use approximations. Depth and block-compressed
surface locking are unsupported. Some extended formats use approximate
mappings and require title-specific checks. These D3D8 limits do not describe
the separate native NV2A renderer's MSAA support.

`tests/d3d8_smoke` covers resource formats, state translation, gamma, A8 textures
and clear operations. The gamma, A8 and clear checks require a GPU. Additional
GPU helper fixtures are in `tests/gpu_smoke`, `gpu_device`, `gpu_pass` and
`gpu_xfer`. The migration fixtures passed on Windows with D3D12 and Vulkan;
this does not establish complete D3D8 feature or visual parity.
