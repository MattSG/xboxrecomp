# Step 5: Building the Runtime

The port links generated guest code with the toolkit's runtime libraries and
supplies its image, assets, entry point and application policy. Start from
[the Windows starter](../../templates/new-game/) and customize its placeholders;
it is not a complete port or a portable game loop.

## Build the port

The starter adds the toolkit with `add_subdirectory` and links the `xboxrecomp`
umbrella target. Set `XBOXRECOMP_DIR` to the toolkit root, supply the generated
sources and dispatch headers, and build from the port's root:

```sh
cmake -S . -B build -DXBOXRECOMP_DIR=/path/to/xboxrecomp
cmake --build build --config Release
```

On Windows use native CMake and an MSVC/Windows SDK environment. The runtime's
`XBOXRECOMP_RENDERER` cache option selects `D3D11` or `SDL3_GPU`; the latter uses
SDL3 and DXC through the toolkit's CMake dependency helpers. D3D11 requires
Windows. The supplied Windows starter also has Windows-specific host code and
link dependencies; selecting SDL3_GPU alone does not port that executable.

## Initialize the guest and shared host

Follow the current starter and API declarations, rather than copying captured
addresses or a previous port's memory setup:

1. Load and retain the original XBE bytes. Call `xbox_MemoryLayoutInit` and check
   its result; obtain the guest-to-host offset with `xbox_GetMemoryOffset`.
2. Initialize the kernel, configure the port's game/save paths, and initialize
   the kernel bridge. The image supplies section locations and imported thunks.
3. Select the guest-thread policy explicitly. The minimal starter uses
   `XBOX_THREAD_MODE_INLINE`; a host-driven port must arrange its workers,
   message pumping and shutdown around its chosen policy.
4. Call `xbox_HostHardwareInit` after bridge initialization, then
   `xbox_HostGraphicsInit` with the port's window title. Check both results.
5. Initialize guest stack/register state, build dispatch with
   `recomp_dispatch_init`, start the optional watchdog on the guest-running
   thread, and enter the generated entry point.
6. Stop guest workers before tearing down the hardware, kernel and guest memory.
   Release the retained XBE buffer. Follow the same lifetime order on failures.

The memory API distinguishes installed RAM from the backed guest address range.
`xbox_SetTotalRam`, `xbox_SetMapSize` and optional `xbox_ConfigureMainStack`
are pre-initialization choices; none
is an implicit request to alias arbitrary virtual addresses. Use the shared
address helpers for guest pointers and physical IDs. Keep image-specific
configuration and observed memory layouts in the port.

Shared hardware/MMIO handlers model device operations. Mapping zero pages over
register apertures, forcing success from waits, or patching a GPU read cursor to
its writer can conceal missing execution and completion. Such experiments do
not establish a working renderer or belong in generic startup code.

GPU progress must come from executed work. Register a device fence with
`xbox_Nv2aMirrorFence`, then select its executed semaphore releases with
`xbox_Nv2aCommandFence`; registration alone does not publish completion.
`xbox_Nv2aFrameCounter` counts executed non-replay flips. The former
`xbox_Nv2aMirrorCounter` API and clock-driven frame fallback are removed;
ports requiring synthetic pacing own that policy outside the shared runtime.

## Guest-call contracts and diagnostics

Manual integration code must use `recomp_types.h` for the thread-local register
model. Preserve the actual guest calling convention, return values and stack
effects. A guessed stub returning success is not an implementation.

When overriding a guest entry, verify direct calls, indirect dispatch and
same-chunk calls all reach the intended definition. Renaming generated bodies
and callers together can bypass the wrapper. Prefer correcting a shared lifter
or runtime defect once; keep image-address selection and application policy in
the port. See [D3D8LTCG investigation](../technical/d3d8ltcg-device-context.md).

The `RECOMP_ABI_CHECK` switch must reach both generated calls and the kernel's
logger implementation. For a subdirectory build, define it before adding the
runtime, for example:

```cmake
option(RECOMP_ABI_CHECK "Log guest call stack violations" OFF)
if(RECOMP_ABI_CHECK)
    add_compile_definitions(RECOMP_ABI_CHECK=1)
endif()
add_subdirectory(${XBOXRECOMP_DIR} ${CMAKE_BINARY_DIR}/xboxrecomp)
```

Keep matching executable/debug symbols for diagnostics. Test focused synthetic
contracts first, then inspect menus, loading and moving gameplay using isolated
saves. Successful linking, a returned entry point or an escaped wait does not
prove game fidelity. Continue with [iterative debugging](06-debugging.md).
