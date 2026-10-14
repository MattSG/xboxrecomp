# Exact shader lookup and vertex upload retries

The three CPU lookup regressions use the production `spec_find`/`spec_request`
helpers. They check every byte of a key, a deliberately selected hash-bucket
collision, queued/claimed/ready/failed transitions, publication from another
thread, all 4096 occupied slots, live-queue saturation/retry, prewarm promotion,
deduplication and table allocation replacement. An EMPTY slot is never retained
as a negative lookup result.

The five GPU regressions run on the renderer's selected D3D11 adapter. They feed
valid NV2A instructions (`MOV oPos,v0; MOV oD0,c[n]; END`) and real float
constant arrays into the normal interpreted vertex shader, then inspect actual
render-target pixels. Red/green/blue outputs independently prove that the new
program or constants were consumed. Cases cover:

- Initial version zero, including failures of the initial program, constant and
  per-draw buffer Maps followed by successful retries of that same version.
- Failed changed program and changed constants Maps, including successful
  program upload followed by failed constant upload.
- Failed per-draw vertex constants, with no stale-offset draw.
- Real raw/index ring DISCARD wraps, failed raw/index Maps and subsequent
  NO_OVERWRITE reuse. Abandoned uploads never advance past an undrawn span;
  a wrap still resets its cursor to the start of the new backing allocation.

For worker scheduling and all four specialized shader variants, also run the
existing `tests/nv2a_ps_prewarm` GPU/scheduler suite. Shader bytecode SHA keys,
manifest schema/family identity and graphics output are unchanged.

From the game repository after loading `scripts/enter-env.ps1`:

```powershell
cmake -S tools/xboxrecomp/tests/nv2a_shader_lookup -B conformance_tmp/stutter-shader-lookup-test -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build conformance_tmp/stutter-shader-lookup-test
ctest --test-dir conformance_tmp/stutter-shader-lookup-test --output-on-failure
./conformance_tmp/stutter-shader-lookup-test/nv2a_shader_lookup.exe benchmark conformance_tmp/texture-stutter-shader-cache
```

The optional benchmark loads a complete manifest using the production
source/family/schema/SHA/exact-length validation. It prints the actual record
count and compares the original serial FNV index, an exact previous-key front
cache, the existing scalar four-lane hash as an index, their combination, and
the final production lookup. It alternates candidate order, consumes every
result through opaque noinline functions and reports nine-sample medians; no
timing threshold is part of CTest. No private manifest records are committed.

On the Ryzen 9 PRO 7940HS, the final isolated run used 148 valid 216-byte keys
and 4096 slots. Original FNV lookup cost 176–178 ns. Production combined lookup
cost 6.14 ns for one repeated key, 13.95 ns for four draws per key and 38.21 ns
for all keys alternating. This saves approximately 0.12–0.15 ms per 850 lookups
in these synthetic patterns. It does not establish a whole-game frame-rate or
hitch improvement. The four-lane hash changes only bucket selection; exact
key comparison resolves collisions and the front cache rereads published state.
