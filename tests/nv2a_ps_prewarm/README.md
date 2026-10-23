# Specialised pixel-shader warm-up regression

This suite includes the production renderer, manifest reader/writer and queue.
The optional manifest records at most 512 exact 54-word shader keys; dynamic
factors, fog, alpha reference, texture scales and depth constants remain in the
existing constant buffer. It shares the validated bytecode cache directory and
uses a separate `.psm` sidecar. Schema, field layout, source, shader target,
entry-point/variant family and compiler flags determine its namespace.

Build from the game repository after entering `python scripts/devenv.py --shell`:

```powershell
cmake -S tools/xboxrecomp/tests/nv2a_ps_prewarm -B conformance_tmp/stutter-ps-prewarm-test -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build conformance_tmp/stutter-ps-prewarm-test
ctest --test-dir conformance_tmp/stutter-ps-prewarm-test --output-on-failure
```

The ten tests cover canonical little-endian round trips at the 512-record
limit; malformed magic/schema/record length/count/reserved fields/family/SHA;
truncation and trailing bytes; empty manifests; cache-directory override;
shader/cache/prewarm bypasses; invalid variants; duplicate imported keys;
live-priority promotion; queue saturation and retry; concurrent atomic file
publication; persistence while new keys arrive during blocked file I/O; save
throttling; and two workers claiming
promoted jobs while the render thread looks up the same entries. The GPU tests
compare 32 complete 16x16 images byte for byte with the generic shaders: all four
flat/W-depth variants, basic diffuse output and a register-combiner program,
with dynamic factors and alpha reference changed after shader creation. They
also exercise missing bytecode, unwritable-cache lazy compilation and idle
worker persistence of a newly observed key.

The constant-buffer regression verifies every byte of the 768-byte production
pixel state on the GPU, unchanged-state reuse, initial Map failure and retry,
and retention of the previous successful state after a failed new upload.
The shader-image comparisons use the same upload-reuse path as gameplay.
`nv2a_ps_prewarm constants_bench` additionally reports seven interleaved pairs
of 20,000 repeated uploads versus exact-state reuse. It times CPU/API work only;
GPU buffer readback verifies each result outside the timed loops. Timing is
reported without a pass threshold and does not measure whole-frame speed.

The image comparison is exact. A separate mathematical sanity check allows one
8-bit unit for raster interpolation and UNORM conversion (centroid diffuse
green=0.5 can produce 127 where flat shading produces 128).

Warm-up is enabled by default and uses the existing two workers below normal
thread priority. Live jobs win whenever a worker claims its next job; an
already-running compilation is not interrupted. Shader/cache/prewarm overrides
disable optional metadata I/O. No draw performs filesystem I/O. Workers copy a
bounded snapshot under the queue lock, then checksum and atomically replace the
manifest outside it, at most once per second. Failures leave generic/lazy
rendering available. A force-killed process can lose its last unsaved keys;
those keys are learned again normally.

The first learning run cannot benefit from unseen states. Later launches can
create known shaders during startup, including recompiling missing bytecode.
This does not prewarm the driver's first draw or guarantee zero hitches. Compare
equivalent routes against a warm bytecode cache with `RECOMP_PS_PREWARM=0` when
measuring the benefit. The normal shader compiler instrumentation remains opt-in
through `RECOMP_HITCH_TRACE`.
