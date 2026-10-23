# Runtime support and validation

Runtime support is specific to the path, backend and contract exercised.
A stub, register definition or successful build does not establish hardware
fidelity. Keep title priorities, captures and gameplay acceptance in the port.

## Locate the implementation

| Area | Source | Focused checks |
|---|---|---|
| Kernel, guest memory and ABI | [src/kernel](../../src/kernel/) | [tests/kernel_dispatch](../../tests/kernel_dispatch/) |
| NV2A register/MMIO model | [src/nv2a](../../src/nv2a/) | [tests/memory_layout_posix](../../tests/memory_layout_posix/) |
| Pushbuffer rendering | [src/video](../../src/video/) | [tests/nv2a_surface_sync](../../tests/nv2a_surface_sync/) |
| D3D8 compatibility | [src/d3d](../../src/d3d/) | [tests/d3d8_smoke](../../tests/d3d8_smoke/) |
| APU and DSP | [src/apu](../../src/apu/) | [tests/apu_mixdown](../../tests/apu_mixdown/) |
| Controller backend | [src/input](../../src/input/) | [input integration](../../src/input/README.md) |

These are entry points, not a coverage matrix. In particular, the D3D8 wrapper
and pushbuffer renderer are distinct paths; testing one does not prove the other.
Format mappings may approximate channel interpretation or sample behavior;
source tables and synthetic conversion checks do not prove visual parity.

## Record support claims

For each claim, identify the revision, API/path, host/backend, test input,
expected result and observed result. State unsupported branches and known
approximations. Keep build proof, synthetic contracts, GPU readback and actual
gameplay proof distinct. A bounded driving check does not certify all scenes,
audio, timing or full hardware behavior.

Use the relevant test project's CMake configuration and CTest entries for
reproducible checks. Consult [conformance testing](conformance-testing.md) for
translator contracts. Report missing coverage as unverified rather than
inferring completeness from enum values, method counts or stub return codes.
