# Memory regressions

The guest heap and the memory bridge, checked against the bugs they had. Like
`tests/kernel_bridge`, it needs no title and no game files: it runs the real
memory layout on the small synthetic XBE from `tools/conformance` and calls the
kernel through the thunk dispatcher.

```
cmake -S tests/memory_regressions -B build/memory-regressions -A x64
cmake --build build/memory-regressions --config Release
ctest --test-dir build/memory-regressions -C Release --output-on-failure
```

One test per mode, one process each, because the switches are read once and cached:

| Test | What it checks |
|---|---|
| `memory_regressions_default` | No switch set. Freed heap blocks are split on reuse, contiguous memory is reused, and an explicit high base is not honoured. |
| `memory_regressions_model_churn` | No switch set. Reuses an unaligned free tail for a page-aligned request, coalesces its fragments, and runs 200 model-sized temporary allocations interspersed with small persistent objects without exhausting the heap or corrupting those objects. |
| `memory_regressions_scene_churn` | `RECOMP_HEAP_RECLAIM=1` on a 128 MB map. Repeats 24 whole-roster scene loads using commit-only flags and synthetic non-page-aligned block sizes, releasing through the NT bridge; addresses stabilize, reused bytes are zero, and live neighbours remain intact. |
| `memory_regressions_heap_reclaim` | `RECOMP_HEAP_RECLAIM=1`. The first table below. |
| `memory_regressions_ext_vma` | `RECOMP_EXT_VMA=1`. The second table. |
| `memory_regressions_ext_vma_128` | `RECOMP_EXT_VMA=1` on a 128 MB map. The tracker stays off. |
| `memory_regressions_reserve_heap` | Retail RAM on a 128 MB map. Heap growth preserves above-RAM reservations, partial commit retains its address, and the existing reserve release/decommit no-ops retain ownership. Later reservations skip issued heap blocks. |

## TLS directory and storage

The TLS cases add a synthetic data section where fixed bootstrap buffers used
to live. A template larger than 512 bytes must retain its input section, copied
data and zero-fill, with separate adjacent per-thread storage in an owned heap
allocation. Invalid directory bounds, reversed template bounds and overflowing
sizes must fail initialization and release the guest mapping.

The image-range cases retain the original text section and add a synthetic data
section above 64 MiB with a configured 128 MiB mapping. Its initialized bytes
and image extent must load. A section whose end wraps the 32-bit address space
must fail initialization and release owned mappings before any section write.
Wrapped raw offsets, invalid section-table pointers and short headers likewise
fail rather than reading outside the supplied image.

## Heap reuse and `RECOMP_HEAP_RECLAIM`

Splitting reused blocks, coalescing neighbours and contiguous reuse are always
enabled. `RECOMP_HEAP_RECLAIM` additionally enables NT release/decommit behavior.

Each check fails without the change and passes with it.

| Check | Without it |
|---|---|
| A 16-byte request after a 2 MB free takes 16 bytes | The whole 2 MB block, so a few thousand small requests drain the heap. |
| The rest of that block is still available | Not reachable: it went with the first request. |
| Freeing three neighbours in the order first, second, third merges all three | The third never joins: the merge of the first two left an empty slot that the neighbour search stopped at. |
| `NtFreeVirtualMemory(MEM_RELEASE)` on memory the heap supplied returns it | `STATUS_UNSUCCESSFUL`. The 32-bit guest slot went to the host `VirtualFree` as a pointer, so nothing came back. |
| A page `MEM_DECOMMIT` then `MEM_COMMIT` again reads as zero; its neighbours keep their data | The old contents: both calls were no-ops on heap memory, while the console hands recommitted pages back zeroed. |
| `MmFreeContiguousMemory` gives a contiguous block back and the next request of that size reuses it | Nothing came back: the address went to the general heap, which does not own it, so a title that frees and reloads a scene runs the 64 MB window out. Contiguous memory is reclaimed in default mode too. |

## `RECOMP_EXT_VMA`

A title that reserves a specific address above RAM (`0x74000000` up
on a 64 MB map) and checks it got that address back. Without the switch the
request is satisfied from the heap at a different address; the title notices and
fails, and touching the address it asked for faults.

| Check | Without it |
|---|---|
| Reserve at `0x76000000` returns `0x76000000` | The heap's address (`0x00F90000` in the run above). |
| Commit inside it succeeds and the memory is usable | Access violation: nothing backs that host address. |
| Query reports the committed part `MEM_COMMIT`, the rest `MEM_RESERVE`, one allocation base | "Everything above RAM is free", so a scanner is handed live memory. |
| Reserving the same range again is `STATUS_CONFLICTING_ADDRESSES` | A second grant. |
| An address in the slots above RAM is refused, and queries report it reserved | Those slots are not backed by RAM. Before they were left unmapped they aliased low memory, and a reservation there zeroed live `.rdata`. A scanner that is told "free" retries the same address forever. |
| `base = 0` still comes from the heap | (unchanged) |
| Release, then query, then reserve again works, and the memory is zero | (n/a) |

The 128 MB mode guards the case where the slots above RAM already reach past user space
(`128 MB * 29` is 3.7 GB): there is nothing above them to track, and a tracker
that subtracted anyway would ask for a range of negative size. It cannot fail on
v0.12.0, which has no tracker; it exists for this tracker.

The tracker reserves host memory at guest address plus the layout's offset, and
stops at the first thing the host has there. On Windows with the image mapped at
`0x10000` that is `KUSER_SHARED_DATA`, so the tracked range ends at
`0x7FFD0000`, not `0x7FFE0000`; the startup line says where it ended. If the host
range is taken earlier still, link the title with `/DYNAMICBASE:NO` so the
loader does not place modules in the low 2 GB.

## What it does not cover

Titles. It shows the allocator and the tracker contracts in each mode; it does
not show any particular title runs better with them. Which titles free enough for the switch to matter, and
whether a title's addresses matter to it, is a per-title question.

The block table's lock is not exercised: nothing here allocates from two
threads. `tests/kernel_dispatch/reserve_heap_ownership_test.c` separately checks
both allocation orders, exact alignment gaps, arithmetic overflow, and eight
simultaneous reservation/heap allocation threads.

The `memory_cpp_linkage` case compiles a C++ consumer and links all nine late
thread/worker APIs to their production C definitions. It checks public linkage,
not thread scheduling or allocator behavior.

The stack cases place initialized image data at the former fixed stack base,
check page-rounded XBE stack commit, and require disjoint main/callback/heap
storage. Port overrides reject invalid arguments, image overlap and insufficient
backing; runtime changes are refused. The configured above-RAM case also checks
reservation exclusion, shutdown/reinitialization and stale positive-cache
invalidation when switching back to the default layout.

`memory_thunk_*` uses synthetic XBE headers and sections, without title assets.
It covers retail/debug tables above 64 MiB, absent/empty tables, invalid markers,
zero ordinals, unbacked pointers, missing terminators and the supported slot
limit. Explicit NULL-separated spans and a noncanonical source ordinal exercise
port selection/remapping without guessing neighboring imports. Reinitialization
and explicit clearing must discard old import metadata.

Remapped function/data cases compare native export pointers to canonical
resolution and verify bridge function cleanup or guest data-export addresses.
A second mapping at the target ID catches accidental double translation;
zero/missing/out-of-range entries and disabled/negative-count maps keep identity.
These cases cover implemented exports, not all native/bridge export coverage.

Launch-page cases verify all 4096 bytes of caller-supplied boot input, native
pointer identity, guest alignment/copy independence and absent-page behavior.
Setting RECOMP_CMDLINE alone must not create generic runtime launch data.

The kernel-data auto case loads image bytes at the former fixed export page;
bridge initialization must preserve them, place exports after the image and
exclude the new page from stack/callback/heap ownership. Older raw-scratch
fixtures explicitly select their separate export page.
Explicit kernel-data cases cover accepted overrides, rejected policy changes,
section/header overlap, unbacked storage, an empty-section image, zeroed fresh
storage, shutdown clearing and restoration of automatic placement. A C++
consumer links the configuration API. These are storage-ownership checks, not
full kernel-data export fidelity checks.

Identity cases compare configured and unconfigured hardware/version/credential
exports between native resolution and the guest page. Every byte of hardware,
version, three keys, alternate keys and public-key storage is checked; the
runtime must ignore credential environment variables. This does not prove any
hardware identity or cryptographic/kernel behavior.

Counted-string cases cover port strings/topology, embedded NULs, nonterminated
input, long paths, all 65535 counted bytes, absent/empty input, invalid counts or
NULL data, and exhausted guest heap. Guest STRING descriptors use 32-bit VAs;
Length/MaximumLength and bounded owned capacity match the valid native input.

Worker-stack cases cover more than eight live stacks, twelve simultaneous
allocations/releases, forged/interior/duplicate tops, minimum policy, explicit
sizes/page rounding, unbacked policy, overflow and restored image defaults.
The C++ consumer links both new APIs. Reset/shutdown requires quiesced users.

Bounds/kernel-stack cases check exact intervals, optional/zeroed outputs,
released workers/callback leases, shutdown, ordinary-heap exclusion and strict
kernel/worker allocation kinds. Kernel-stack size remains independent of worker
minimum policy; deletion requires the matching pair and cannot free an ordinary
heap block. C++ linkage covers the bounds and kernel-stack helper APIs.
