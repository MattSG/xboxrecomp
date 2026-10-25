# Runtime integration

The runtime provides reusable host implementations, guest bridges and device
models for statically recompiled Xbox code. Ports own their generated code,
application hooks, captured addresses/data, assets and compatibility policy.
Individual subsystems remain partial; successful linking is not game fidelity.

## Build and link

Use the top-level [build guide](../README.md) for dependencies and supported
renderer/platform combinations. `XBOXRECOMP_RENDERER=D3D11` selects the Windows
D3D11 implementation; `SDL3_GPU` selects SDL3 GPU, with runtime backend selection
such as Vulkan or Direct3D 12 where supported.

Embed the toolkit with CMake and link the umbrella target:

```cmake
add_subdirectory(path/to/xboxrecomp xboxrecomp-build)
target_link_libraries(my_port PRIVATE xboxrecomp)
```

The top-level [CMakeLists.txt](../CMakeLists.txt) defines the current module and
platform dependencies. Avoid relying on a captured library count or output path.

## Port responsibilities

Provide `recomp_lookup` and `recomp_lookup_manual` to resolve guest entry points
and overrides. Use the shared [register/call model](../docs/technical/register-model.md)
and runtime template; guest addresses and host pointers are distinct.

Initialize memory from a complete XBE, check initialization results, and keep
its bytes alive until shutdown. Configure port policy before guest execution.
Then initialize native kernel imports and guest bridges, select asset/save roots
and bring up the host/device services the port uses. Consult the
[kernel guide](kernel/README.md) and [memory ownership guide](../docs/technical/memory-layout.md)
for exact configuration and lifetime contracts. Host services are declared in
[host/xbox_host.h](host/xbox_host.h); ports still own the application entry and
shutdown lifecycle. A minimal API sequence is not menu/loading/driving proof.

## Modules

- [Kernel](kernel/README.md): guest imports, memory ownership, files, threads,
  synchronization, objects, HAL and runtime helpers.
- [D3D compatibility](d3d/README.md), [NV2A](nv2a/README.md) and [video sources](video/CMakeLists.txt):
  graphics/device translation, renderer selection, scanout and media output.
- [Audio compatibility](audio/README.md) and [APU](apu/README.md): host audio,
  voice processing and DSP support.
- [Input](input/README.md), [USB sources](usb/CMakeLists.txt) and
  [network sources](net/CMakeLists.txt): host controllers and guest device support.
- [Platform sources](platform/CMakeLists.txt) and [host sources](host/CMakeLists.txt):
  host primitives, MMIO routing and service bring-up.

Linked module guides and tests describe their own scope. Do not infer complete
kernel/device semantics, cross-platform parity or application compatibility
from the presence of a route or library target.
