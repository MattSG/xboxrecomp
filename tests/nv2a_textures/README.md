# Texture cache and upload checks

From the XboxRecomp checkout:

```sh
cmake -S tests/nv2a_textures -B build/textures
cmake --build build/textures --config Release
ctest --test-dir build/textures -C Release --output-on-failure
```

The suite checks CPU decoding and mip reduction, resource/view retries,
palette identity and bounds, guest mip retention, and generated mip readback.
Native D3D11 tests also compare BC sampling with a decoded BGRA reference.
SDL GPU tests cover BGRA readback and retained compressed bytes; compressed
byte equality alone does not prove shader sampling parity.

The renderer preserves supplied guest mip levels by default. Set
`RECOMP_COMPLETE_MIPS=1` before the first texture upload to complete partial
2D/cube chains down to 1x1. Only literal `1` enables this enhancement; the
setting is cached and cannot be changed during a renderer session. Single-level
and volume textures are unchanged. Guest memory bounds remain based on supplied
levels. Completion can require BGRA conversion instead of native BC resources.
CTest selects this option explicitly for generation cases and clears it for
default-policy cases.

On Windows, use an MSVC development shell. Select an SDL GPU backend with
`RECOMP_SDL_GPU_DRIVER=vulkan` or `direct3d12`. Non-Windows builds omit D3D11 cases.
For a renderer snapshot comparison, configure with
`-DNV2A_TEXTURE_RENDERER=/absolute/path/to/nv2a_d3d11.c`.
Optional executable modes `benchmark_decode` and `benchmark_native_bc` report
local CPU costs without timing pass/fail thresholds.
