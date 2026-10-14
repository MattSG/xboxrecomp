# Sampler requests and supplied mip levels

This suite includes the actual renderer and selects its usual high-performance
D3D11 adapter. It does not replace the GPU or decode sampled pixels on the CPU.

The four CTests cover:

- 224 exact descriptor comparisons and 28,672 pairs of native GPU samples,
  including four stage requests, point/linear/anisotropic filtering, wrap,
  mirror, clamp, border, signed bias, LOD limits and render-target reductions.
  Changes to unused input bits conservatively refresh a request. A point-sample
  check also asserts a known green pixel independently of the reference path.
- Actual eviction from the full 512-entry sampler table. The old sampler is
  retained only by the test reference; the request must validate its table slot,
  recover the original descriptor and produce the same GPU samples.
- A failed sampler creation followed by a successful retry of the exact same
  request, with matching native GPU samples.
- Supplied mip counts against the old independent `tex_layout` calculation,
  including default/clamped/cube/linear layouts, same-kick cache reuse, invalid
  layouts and guest ranges, failed SRV creation, new and existing mip-allocation
  failures, disabled/missing stages, `control0=0`, and actual render-target,
  complete render-target mip-chain and reduction aliases. Pipeline checks read
  back the sampler and shader-resource slots from the real D3D11 context.

Build and run from the game repository after loading `scripts/enter-env.ps1`:

```powershell
cmake -S tools/xboxrecomp/tests/nv2a_samplers -B conformance_tmp/stutter-samplers-test -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build conformance_tmp/stutter-samplers-test
ctest --test-dir conformance_tmp/stutter-samplers-test --output-on-failure
./conformance_tmp/stutter-samplers-test/nv2a_samplers.exe benchmark
```

The optional benchmark has no pass/fail timing threshold. It warms actual
samplers on the selected adapter, alternates reference/candidate order, uses
opaque noinline entry points and consumes every result. It reports the median
of nine two-million-call samples for one through four alternating stages.
It measures CPU request lookup, rather than D3D sampler creation or a draw.

On the Ryzen 9 PRO 7940HS with NVIDIA RTX 2000 Ada, the first isolated run
measured 6.49–7.36 ns per original warm request versus 3.80–4.51 ns with stage
request reuse. This is a small saving: about 0.009–0.010 ms per 3,200 lookups.
No whole-game frame-rate or hitch improvement follows from this microbenchmark.
The renderer also removes the second layout calculation for non-render-target
textures: `texture_get_with_levels` returns the count from its existing layout
calculation before cache hits or upload failures; other alias paths stay intact.
Full content hashing and per-kick validation are preserved.
