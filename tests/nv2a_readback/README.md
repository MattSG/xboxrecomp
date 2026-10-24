# Batched GPU readback regression

The renderer ends one D3D11 EVENT query after all staging copies and submits the
batch with one explicit `Flush`. An unfinished nonblocking poll calls
`GetData(DONOTFLUSH)` once instead of repeatedly attempting to map every staging
texture. It maps the real resources only after event completion; a busy Map
still delays acknowledgement. Query allocation or GetData failures retain the
existing Map fallback, and blocking staging-slot reuse keeps its original path.
Pixel conversion, downsampling, format alpha and later CPU-row merge are intact.

Microsoft documents EVENT completion as `S_OK` plus a true BOOL, and
`DONOTFLUSH` as preventing implicit command-buffer flushes:

- [D3D11_QUERY_EVENT](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/ne-d3d11-d3d11_query)
- [GetData](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-getdata)
- [D3D11_ASYNC_GETDATA_DONOTFLUSH](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/ne-d3d11-d3d11_async_getdata_flag)

Build after entering the game repository's a shell with the MSVC environment (vcvars64.bat):

```powershell
cmake -S tools/xboxrecomp/tests/nv2a_readback -B conformance_tmp/stutter-readback-test -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build conformance_tmp/stutter-readback-test
ctest --test-dir conformance_tmp/stutter-readback-test --output-on-failure
```

The eleven tests execute actual GPU copies and verify every logical guest pixel
and pitch-padding byte. Four MSAA cases (1/2/4/8) cover an eight-surface batch,
16/32-bit formats and late CPU rows. Controlled unfinished query responses prove
that 64 polls issue exactly 64 GetData calls, zero Maps and no additional Flush;
then normal GPU completion supplies the expected pixels. Other tests cover
event allocation retry, GetData failure fallback, busy Map retry after real
event completion, dropped/resized surfaces at reused addresses, and downsample
view allocation failure retaining dirty pixels for a successful next-batch retry.

The opt-in `RECOMP_HITCH_TRACE` pending marker reports `poll_count` and
`max_poll_gap_ms`. Timing begins on the first unfinished poll, after deliberate
vblank pacing. The maximum gap compares poll start times, so it exposes pauses
in polling; it does not independently measure GPU execution time. Separate
`readback_poll`/`readback_blocking` markers still measure slow CPU calls. Trace-off
regression verifies zero QPC calls during readback.

For individual calls above the hitch threshold, `readback_parts` separates
query, mapping, pixel-copy and unmapping wall time, with surface/row counts.
Measurements are buffered until the call finishes so diagnostic output is
excluded from those subphases. These clocks are also trace-only. A long wall
time can include thread preemption; use thread CPU time or sampling before
attributing it to a blocking driver call.

The optimization removes redundant readiness/driver calls. It does not remove
GPU copy work or guarantee a pending batch cannot stall. A complete-frame timing
claim needs a clean gameplay comparison without concurrent tests or builds.
