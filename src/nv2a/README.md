# xbox_nv2a

Partial NV2A register/MMIO model adapted from [xemu](https://github.com/xemu-project/xemu).
Keep the source attribution and applicable [license notices](../../LICENSES/README.md).

## Integration

Link the `xbox_nv2a` CMake target. Its public declarations are in
[nv2a_state.h](nv2a_state.h) and [nv2a_mmio_hook.h](nv2a_mmio_hook.h).

For direct register access, call `nv2a_init_standalone` with caller-owned VRAM
and RAMIN buffers and their sizes. Keep those buffers alive for the device's
lifetime and check the returned pointer. This is a process-wide singleton;
subsequent initialization returns the existing state. There is no teardown API.
`nv2a_mmio_read` and `nv2a_mmio_write` take offsets within the GPU register
aperture, not absolute guest virtual addresses. Register definitions live in
[nv2a_regs.h](nv2a_regs.h).

For host fault handling, `nv2a_hook_init` creates VRAM/RAMIN storage only when
no standalone state exists. It reuses existing state and releases new storage
if state initialization fails. Initialization is quiesced and process-wide;
its legacy offset argument is unused. It does not register a host exception handler: the host
must validate the fault address and route supported GPU faults to
`nv2a_hook_handle_mmio`. A false result leaves the fault unhandled; the host
chooses its error policy. Instruction decoding is host-specific and partial.
The non-x86-64/non-Windows branch supplies unsupported stubs.

VRAM aliasing belongs to the memory-layout/mapping layer; this hook handles
register accesses only. PVIDEO snapshot/consume helpers coordinate with the
hook's PVIDEO access lock. Direct register calls do not acquire that lock.

## Boundaries

- `nv2a_core.c` dispatches register blocks, tracks interrupt/register state and
  provides partial device behavior. Unsupported blocks use stub handlers.
- PFIFO/PGRAPH state here is not a complete command processor or renderer.
  `pgraph_method` shadows methods and counts selected operations. PCRTC raster
  reads advance a synthetic counter rather than emulating scanline timing.
- Pushbuffer execution lives in [src/kernel](../kernel/); pixel production
  lives in [src/video](../video/). Those paths have their own support limits.
- [qemu_shim.h](qemu_shim.h) provides standalone compatibility definitions;
  it does not supply the full QEMU device/bus lifecycle.

Application patches, captured command streams and visual policy belong to
consumers. Optional shader replacement is described in
[shader-overrides.md](../../docs/technical/shader-overrides.md).
This module's presence does not establish complete NV2A hardware fidelity.
