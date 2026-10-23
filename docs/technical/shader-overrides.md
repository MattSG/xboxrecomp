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
