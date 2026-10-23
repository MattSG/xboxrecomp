# Xbox kernel integration

The runtime provides native kernel facades, guest import bridges and memory
ownership helpers. Routed exports include partial implementations and stubs;
routing coverage alone does not establish Xbox kernel fidelity.

## Integration boundaries

- [kernel.h](kernel.h) declares native host-facing APIs and callback types.
- [kernel_bridge.c](kernel_bridge.c) decodes guest arguments and translates
  guest addresses/handles. A native prototype is not a guest marshaling contract.
- [kernel_thunks.c](kernel_thunks.c) resolves canonical native exports and owns
  the optional caller-supplied ordinal remap.
- [xbox_memory_layout.h](xbox_memory_layout.h) declares memory, stack and device
  ownership APIs. [Memory integration](../../docs/technical/memory-layout.md)
  describes initialization, lifetime and configuration limits.
- File, path, thread, synchronization, memory, object, I/O, HAL, runtime and
  crypto modules implement individual native routes. Inspect each route and
  its focused tests before relying on its semantics.

## Image and application policy

Supply the complete XBE to `xbox_MemoryLayoutInit` and check its result. Keep
its buffer alive until shutdown. Image sections, executable bounds and default
stack size come from the XBE. The active `XBOX_STACK_*` and kernel-data address
are runtime values. Do not edit shared headers to install a title memory map.

A port owns extra stack capacity, fixed placements, virtual hardware identity,
launch input and application/device layouts. Configure supported overrides
before initialization and check their results. Keep captured addresses and
application hooks in the port. Quiesce guest/device execution before shutdown
or reinitialization; release allocations through their matching ownership API.

Guest memory accesses use the runtime offset and shared register declarations.
Guest virtual addresses, host pointers and virtualized physical IDs are distinct.
Use the memory mapping/resolution APIs rather than a guessed address mask.

## Imports and data exports

The loader validates the XBE's terminated thunk table. A standalone host must
supply mapped storage and select its exact import span explicitly. Configure
any verified ordinal remap before native kernel and bridge initialization;
canonical native lookup does not apply that map again. Consult the memory guide
for configuration lifetime and invalid-input behavior.

Call `xbox_kernel_init` to initialize native exports, then
`xbox_kernel_bridge_init` to publish guest routes and data. Kernel data uses
owned image-following placement by default; fixed placement belongs to port
configuration. An export-directory audit checks routes and argument sizes,
not implementation completeness.

## Files and threads

Initialize path translation with the port's asset/save directories using
`xbox_path_init`. Device and drive paths are translated by `kernel_path.c`;
Windows uses UTF-16 host paths and POSIX uses UTF-8 paths. Save/cache mappings,
partition-device handling and symbolic links have separate route behavior.

Guest creation uses dispatch, owned stacks and per-thread CPU/TIB state.
Native `xbox_PsCreateSystemThread`/`Ex` accepts host callbacks: a start routine
receives one context; an optional system routine receives the start callback
and context. Native extension/TLS/debugger requests are unsupported and
reported. See the header and [thread checks](../../tests/kernel_bridge/README.md)
for the tested scope; guest suspension/ID/TLS and object fidelity remain open.

## Verification

Run [kernel unit checks](../../tests/kernel_regressions/CMakeLists.txt),
[bridge checks](../../tests/kernel_bridge/README.md) and
[memory regressions](../../tests/memory_regressions/README.md) for their specific
contracts. `tools/kernel_audit/test_bridge_ordinals.py` checks export routing.
None replaces port-level menu/loading/driving validation or proves complete
I/O, synchronization, object, exception or hardware semantics.
