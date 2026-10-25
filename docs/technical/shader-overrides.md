# Port-owned NV2A shader fragments

A port can select an absolute directory with `XBOXRECOMP_NV2A_SHADER_DIR` before
adding the toolkit or including its shader CMake helpers:

```cmake
set(XBOXRECOMP_NV2A_SHADER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/shaders/nv2a")
add_subdirectory(path/to/xboxrecomp xboxrecomp-build)
```

A file whose name matches a fragment in `NV2A_HLSL_PARTS` replaces that fragment;
missing files retain the shared source. This applies to D3D11's embedded HLSL
and to SDL GPU's generated compile units, including SPIR-V and Windows DXIL.
An unset directory selects all shared fragments. Relative or missing directories
are rejected. Configuration is build-time policy, not a runtime setter.

The port owns replacement semantics and must preserve required entry points,
constant-buffer layouts, resource bindings and stage interfaces. Local helper
includes remain relative to the selected file. HLSL/HLSLI files under the
replacement directory are build dependencies; adding/removing files triggers
selection again. External includes outside that directory need their own build
dependency handling. Do not infer GPU fidelity from compilation alone.

The [CMake fixture](../../tests/cmake_sdlgpu/CMakeLists.txt) compiles shared and
caller-owned source selection. Configure once normally and once with
`-DXBR_TEST_SHADER_OVERRIDE=ON`, then build and run CTest in each build directory.
It checks selected/fallback compile-unit paths and the embedded D3D11 source;
DXC builds the selected shader to SPIR-V and, on Windows, DXIL. These are source
selection/build checks, not pixel or runtime compatibility tests.

## Optional depth conversion

`nv2a_y16d.hlsl` is a caller-owned optional fragment; it has no shipped fallback.
When present, both shader helpers define `XBOXRECOMP_DEPTH_VIEW_SHADER` for the
renderer and compile its `y16d_ps` fragment entry. Without it, a draw that reads
a cached depth surface as Y16 is skipped with a diagnostic. It does not receive
fabricated high words or silently read a stale RAM copy instead.

The extension provides a depth source at texture slot 0 and a single-sample
R16G16B16A16_UNORM target twice the source width. It uses the shared binding and
clear-buffer declarations. `y16d_ps(float4 p : SV_Position)` returns float4
colour. D3D11 builds single-sample and `Y16_MS=1` variants; SDL GPU uses a sampled
Texture2D depth source. `ccol.z` carries host source texels per guest pixel (zero
if the guest width is unavailable). It is scale metadata, not a sample shift.
The caller owns any packing, reinterpretation and alignment semantics. This
extension does not implement complete packed depth/stencil reads by itself.

The surface-sync fixture checks absent converters without allocating a proxy or
issuing an unsupported draw. With a converter, it can read an actual converted
GPU texel at a 2x depth boundary. `depth_view_shifted` checks the quarter-pixel
alignment used by a caller shader; this is alignment proof, not packed word or
stencil-preservation proof.
