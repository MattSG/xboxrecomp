# Native graphics quality regressions

The suite includes the production renderer and uses its selected D3D11 adapter.
It compiles the actual HLSL; no renderer or GPU results are simulated.

- `panorama`: 128 wrapped sample positions, all RGBA channels, against an
  independent 16-texel cardinal cubic reference. It verifies ordinary stage-zero
  and stage-one sampling against bilinear reference pixels. Alpha stays within
  the authored range. Float texture fixtures isolate filtering from BC decoding.
- `gates`: inspects the constant buffer produced by real pipeline setup, checking
  title opt-in and exact panorama selection while ordinary, cube, nonzero-stage,
  short-mip, point-filtered, blend and depth-tested draws retain their sampling.
- `depth`: checks every packed high-word depth texel from the actual depth view
  at scales 1/2/4/8, both single-sample and 4x MSAA, against independently indexed
  depth values. Covers the left boundary clamp and all color/alpha channels.
- `placement`: captures the production vertex shader's output through native
  stream output. Checks full-screen bounds, integer menu vertices, fractional
  boundaries, perspective geometry, and offscreen pixel-zero snapping.

From the game repository with its portable toolchain loaded:

```powershell
Set-ExecutionPolicy -Scope Process Bypass -Force
python scripts/devenv.py --shell
cmake -S tools/xboxrecomp/tests/nv2a_graphics_quality -B conformance_tmp/graphics-quality-test -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build conformance_tmp/graphics-quality-test
ctest --test-dir conformance_tmp/graphics-quality-test --output-on-failure
```

The cubic comparison allows 0.004 per float channel for hardware bilinear weight
precision. Packed depth permits one 16-bit unit for depth-store rounding. These
checks establish sampled pixels and alignment, not whole-game visual appearance
or frame-rate improvements.

The `volume` case exercises PROJECT3D and DOT_STR_3D through each production
renderer and shared combiner shader. It checks every slice of a compressed
volume on the GPU, W border addressing, BC3 alpha decoding, rectangular 3D
Morton order and mip offsets. These fixtures establish texture translation;
they do not establish complete in-game headlight or streetlight rendering.

The `y16` case checks the NV2A Y16 byte-lane mapping `(1, high, low, 1)` in
CPU texture decoding and GPU colour-target reinterpretation. The `depth` case
checks the same byte lanes on packed depth views. This follows the hardware
results linked by [xemu's Y16 correction](https://github.com/xemu-project/xemu/commit/ac72edfc2bc033cd1659123c44dc64f250332a60).
