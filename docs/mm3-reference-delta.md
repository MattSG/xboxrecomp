# MM3 reference delta

Compared read-only on 2026-08-25:

- reference: `F:\repos\midtown-madness-3-recomp\.reference\xboxrecomp` at `32da238`
- fork-derived working copy: `F:\repos\midtown-madness-3-recomp\tools\source\xboxrecomp` at `8aca66a`

Generic candidates identified from the fork-derived copy include carry/borrow
and `NEG` flags, REP CMPS, sub-register immediate masking, flag snapshots,
ESP-relative indirect-call addressing, and continuation/jump-table handling.
Imported from that generic delta: the carry/borrow fix and focused regression
test (`d55e994`), then the branch's newer shared implementation was extended
to full arithmetic EFLAGS state, width-aware shifts/rotates, and REP string
semantics. These remain title-independent and are covered by the lifter and
bounded fuzz checks.

Not imported: MM3 scheduler/fiber timing, NV2A/D3D device behavior, MM3 address
probes, hard-coded callback traces, MM3 seed files, and generated `m4tmp`
material. These are title-specific or local artifacts. No bulk copy was
performed; remaining MM3-only runtime, timing, graphics, address probes, and
generated artifacts were excluded rather than generalized by assumption.

## Upstream PR review

- `sp00nznet/xboxrecomp#6` (ABI analysis): imported as `a13f6b3`. It is
  generic, feeds the existing ABI pipeline, and falls back conservatively.
- `sp00nznet/xboxrecomp#5` (Halo bring-up): not merged wholesale. Imported
  generic pieces were packed SSE operations, width-aware signed comparisons,
  frameless EBP inheritance, failed indirect-tail balancing, and global x87
  state. Halo-specific kernel ordinals, EEPROM, NV2A, heap, file, and address
  diagnostics were excluded. Flag-snapshot and function-boundary commits
  conflicted with newer fork code and were retained as superseded.
- The clean generated-source glob update from PR #5 (`794bd7f`) was also
  imported; it makes newly split function chunks visible to CMake without a
  hand-edited source list.
