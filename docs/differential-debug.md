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

`python -m tools.diff.fuzz --seed 1234 --cases 100` creates bounded,
replayable arithmetic/flag cases. With `RECOMP_TRACE_ENABLED=1`, consumers
keep 4096 fixed-size runtime records; decode a raw dump with
`python -m tools.diff.trace_decode trace.bin`. The default build emits no
trace calls.

Build resolver metadata from a consumer's function database with
`python -m tools.resolve.build --xbe default.xbe --functions functions.json
--out resolver.json`, then inspect a target using
`python -m tools.resolve resolver.json 0x001E77F3`. Ranges are checked before
function starts, so secondary entries can be reported explicitly.
