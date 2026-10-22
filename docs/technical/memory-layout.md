# Guest memory integration

The runtime loads image sections from the XBE and maintains the guest-to-host
address translation. The port supplies the image and its application policy;
section addresses, engine scratch pointers and captured device layouts are not
portable runtime defaults.

## Initialization and ownership

Call `xbox_MemoryLayoutInit` with the complete XBE buffer and check its result.
Keep the bytes alive for runtime users until shutdown. `xbox_GetMemoryOffset`
provides the guest-to-host offset; generated code uses the shared register and
memory declarations from `recomp_types.h`. Do not substitute an unrelated host
pointer for a guest virtual address.

Installed RAM and backed guest address space are separate quantities. Set
`xbox_SetTotalRam` or `xbox_SetMapSize` before initialization when the port needs
them. A larger virtual range does not increase reported RAM or make unmapped
addresses aliases of live heap storage. Executable/image bounds come from the
loaded sections rather than a fixed code-size ceiling.

Use the runtime heap, contiguous and virtual-memory interfaces with their
corresponding release operations. Physical IDs and guest virtual addresses are
also distinct: use `xbox_PhysicalAddressResolve` and the physical mapping APIs
instead of a title-derived bit mask or guessed alias. Consult
[`xbox_memory_layout.h`](../../src/kernel/xbox_memory_layout.h) and
[`kernel.h`](../../src/kernel/kernel.h) for current contracts.

## Kernel imports

The loader decodes the header's retail/debug thunk pointer and validates a
terminated ordinal-marked run within the actual mapped span, up to the supported
slot capacity. A missing or empty table leaves no configured imports. Invalid
pointers, malformed entries and missing terminators fail initialization.

A port can select an exact known span with `xbox_kernel_set_thunk_address` before
kernel/bridge initialization. NULL gaps are allowed; neighboring words are never
scanned for possible imports. Zero address/count clears selection, and invalid
ranges/counts leave it cleared rather than inferring or clamping a count.
The getter reports accepted selection. Standalone hosts supply their own mapped
storage; initialized layouts check the selected span against their backing.
The loader validates markers without requiring canonical export IDs; the bridge
retains port-configured ordinal remapping. Shutdown retires import metadata.

## Main stack configuration

The main stack defaults to page-rounded XBE stack commit (header offset `0x130`),
with a one-page minimum for an absent/zero value. Placement follows the loaded
image and kernel data. Callback slices follow the whole main stack; an
insufficient callback span disables leases, while insufficient main-stack space
fails initialization and releases mappings.

`xbox_ConfigureMainStack(base, size)` is an optional pre-initialization port
policy. Base and size must be page-aligned; zero base chooses placement and
zero size chooses the image value. `(0,0)` restores both defaults. Invalid or
active-layout requests fail without changing policy. The initializer checks
backing and overlap. Keep inherited fixed placements or extra capacity in the
port. `XBOX_STACK_*` and the heap floor are runtime values, not integer constant
expressions. Stack values and allocator ownership retire at shutdown; callers
must quiesce guest/device execution before reset or reinitialization.

## Thread and application state

The shared runtime owns guest KPCR/TIB storage and image-derived TLS setup.
A port owns any inherited application context chains or engine scratch layout.
Overrides must use the shared thread-local register declarations and preserve
the actual guest call/stack contract.

`xbox_GetKernelMemoryBase` exposes a runtime-owned, initially zeroed 4 KiB kernel
probe page, or NULL if unavailable. It does not expose an emulated kernel PE
image. A port may populate compatibility data before guest execution; it must
not free the view or retain it after memory shutdown. Fabricated header fields
used to steer an application's feature probing belong to that port.

## Devices, diagnostics and shutdown

Use the shared host/MMIO initialization and hardware handlers. Zero-filling a
register aperture or reporting unexecuted work as completed is not a substitute
for the device protocol. Caller-configured capture layouts describe application
structures; raw snapshots are bounded diagnostics, not atomic guest snapshots.

Stop guest workers and hardware execution before releasing memory. Use
`xbox_MemoryLayoutShutdown` for the runtime-owned mappings, including failed
startup cleanup. Test synthetic range/ownership/lifetime cases before judging
loading and moving gameplay. Mapping success alone does not establish fidelity.

See [runtime setup](../pipeline/05-runtime.md) and
[D3D8LTCG investigation](d3d8ltcg-device-context.md) for the adjacent integration
and state-recovery workflows.
