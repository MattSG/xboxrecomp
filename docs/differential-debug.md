# Differential debug cases

Run `python -m tools.diff cases/foo --oracle oracle.json --recomp recomp.json`.
Add `--run-oracle` for the optional Unicorn oracle, or `--oracle-command` for a
trusted x86 engine adapter. Add
`--recomp-command '...'` for a generated-runtime adapter. The adapter receives
`XBOXRECOMP_DIFF_CASE` and `XBOXRECOMP_DIFF_OUT`; this keeps generated game
projects independent of the toolkit's Python process.
Both adapters emit the same ordered checkpoint JSON stream; the comparator
reports the earliest differing field and guest EIP. A case contains guest code,
entry/stop boundary, all general registers and EFLAGS, x87/SSE state when
available, memory pages, and an external-call transcript. The runner adapter
may use Unicorn or another trusted x86 engine; the case format does not depend
on that choice.

For a disposable Unicorn environment on Windows, use
`uv run --with unicorn --with capstone --project . python -m tools.diff.fuzz`;
the dependency is optional and is not vendored.

`python -m tools.diff.fuzz --seed 1234 --cases 100` creates bounded,
replayable arithmetic/flag cases. With `RECOMP_TRACE_ENABLED=1`, consumers
keep 4096 fixed-size runtime records; decode a raw dump with
`python -m tools.diff.trace_decode trace.bin`. The default build emits no
trace calls.
Generated functions also emit an instruction event for every guest instruction
when tracing is enabled, allowing a consumer to refine a function/basic-block
failure to the exact guest EIP without editing generated C.
Call `recomp_trace_set_memory_filter(begin, end)` to retain only generated
memory-read/write events whose guest address is in the selected half-open
range; the same bounded ring stores those events.
Define `RECOMP_DIFF_CHECKPOINT_ENABLED=1` in the generated consumer to fill a
second bounded ring with EIP, EAX/EBX/ECX/EDX/ESI/EDI, ESP, and EFLAGS at each
instruction event. This is the generated-side state stream an adapter can
serialize to the same checkpoint JSON shape as the oracle.
Raw records copied from that ring can be decoded with
`python -m tools.diff.checkpoint_decode checkpoints.bin`.
Pass `--recomp-command` to run a generated-runtime adapter for every case;
`--stop-on-failure` leaves the failing case, oracle, and recomp trace in the
output directory and prints the first divergent checkpoint.

Build resolver metadata from a consumer's function database with
`python -m tools.resolve.build --xbe default.xbe --functions functions.json
--out resolver.json`, then inspect a target using
`python -m tools.resolve resolver.json 0x001E77F3`. Ranges are checked before
function starts, so secondary entries can be reported explicitly.
Bulk indirect-call diagnostics use `python -m tools.resolve.report resolver.json
callsites.json --resolve-all-calls`; unresolved targets retain their nearest
range and conservative provenance classification.

Capture a local replay case from a real XBE and JSON runtime snapshot:

```powershell
python -m tools.capture --xbe default.xbe --func 0x001E77F3 --state state.json --out cases/local/001E77F3
```

The snapshot contains `state`, `end_eip`, and optional `stop`, `memory`, and
`calls`. Captured XBE bytes are marked local-only and must remain ignored; the
manifest records only the XBE hash for repository diagnostics.
