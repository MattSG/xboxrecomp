# Runtime Implementation Guide

The recompiled C code needs a host environment to run. This directory documents the runtime subsystems supplied by XboxRecomp and the boundaries a game port configures.

## The Four Pillars

### 1. Memory Layout (`xbox_memory.c`)

Use `xbox_MemoryLayoutInit` from `xbox_kernel`. It parses the supplied XBE and maps its sections; do not copy another title's addresses or write a second allocator.

See: [../technical/memory-layout.md](../technical/memory-layout.md)

Template: [../../templates/runtime/xbox_memory.h](../../templates/runtime/xbox_memory.h)

### 2. Kernel Shim (`xbox_kernel.c`)

Link `xbox_kernel` and initialize imports from the loaded XBE. Unsupported operations must remain observable; do not replace them with unconditional success. The legacy template include forwards to the real runtime header.

See: [../technical/kernel-replacement.md](../technical/kernel-replacement.md)

Template: [../../templates/runtime/kernel_stubs.h](../../templates/runtime/kernel_stubs.h)

### 3. Graphics Translation (`d3d8_to_d3d11.c`)

The Xbox uses a modified Direct3D 8. You need a translation layer to D3D11 (or D3D12, Vulkan, etc.). This is the largest runtime component.

See: [../technical/d3d-translation.md](../technical/d3d-translation.md)

### 4. Input System (`xbox_input.c`)

Map Xbox controller input to Windows XInput. This is usually the simplest piece — Xbox controllers on Windows are nearly 1:1.

## Build System

We recommend CMake with MSVC. Structure your project as static libraries:

```
src/
├── kernel/     → xbox_kernel.lib
├── d3d/        → d3d_translation.lib
├── audio/      → audio_stubs.lib
├── input/      → input_layer.lib
└── game/       → your_game.exe (links all the above + recompiled code)
```

## Game-Specific Components

Beyond the four pillars, each game needs:

- **Manual function overrides** — hand-written replacements for functions that don't work when mechanically translated (physics, rendering orchestrators, hardware-specific code)
- **Asset loaders** — each game/engine has its own file formats for textures, models, levels, audio, etc.
- **Game-specific workarounds** — vtable guards for corrupted pointers, state machine patches, etc.

These go in the game-specific repo, not in the generic xboxrecomp toolkit.


## Guest thread startup

Both guest thread-create APIs default to spawned host threads with independent
registers and guest stacks. Ports that require the legacy first-call inline
model can call `xbox_SetThreadMode(XBOX_THREAD_MODE_INLINE)` before guest startup.
That is an explicit compatibility choice; normal callers need no selector.


Host callbacks entering guest code use `xbox_worker_stack_alloc` and
`xbox_worker_stack_free` for a guest stack slice. The kernel's DPC/timer,
network, USB and APU paths use this pool; it is not limited to a host-driven
game loop. Guest-created threads use `xbox_AllocThreadStack` instead.

The loader places the callback pool after the image, page aligned, and keeps
ordinary heap allocations outside it. Pool leases are disabled if the complete
pool cannot fit in mapped guest RAM. Shutdown clears placement; loading another
image computes it again. Standalone hosts with their own guest mapping can use
`xbox_worker_stack_reset(base)` before workers start (or after they have stopped)
to supply a mapped pool; zero disables it. Slot count and size are unchanged.


## Renderer policy

`NvOverlayHooks.vertex_alignment` lets a port choose the existing shader's
pixel-zero snap and screen-offset parameters from guest target dimensions.
Without a hook, snapping is enabled and the screen offset is zero for every
target size. Authored pass selection belongs to the port; the shared shader
implements the coordinate operations for both renderers.


The Windows new-game starter uses `xbox_HostHardwareInit` and
`xbox_HostGraphicsInit`, then shuts hardware down before releasing guest memory.
It explicitly selects INLINE for its simple guest-driven loop. A port with its
own host loop selects SPAWN and owns worker lifetime and joining. The starter's
VEH is diagnostic only; shared host setup owns hardware MMIO routing.
