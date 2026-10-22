# Documentation Index

Every document in the toolkit, in one place. The README's
[Documentation](../README.md#documentation) section lists where to start; this
page lists everything, and then the same pages again by the problem in front of
you — because a title under bring-up rarely announces which subsystem it is
stuck in.

## Start Here

- [Getting Started](GETTING_STARTED.md) — End-to-end walkthrough from XBE to a running title
- [Decompilation Guide](DECOMP.md) — Using the toolkit as a function splitter instead: one byte-exact `.s` per function, with signatures and the call graph
- [Runtime Implementation Guide](runtime/README.md) — The four pillars every title needs: memory layout, kernel, graphics, input
- [Contributing](../CONTRIBUTING.md) — Setup, where to focus, code style, how to submit

## Pipeline Guides

One per stage, in the order you run them.

- [Step 1: Extracting and Parsing XBE Files](pipeline/01-xbe-parsing.md)
- [Step 2: Disassembly and Function Detection](pipeline/02-disassembly.md) — including the four ways the detector misses or merges functions
- [Step 3: Function Identification](pipeline/03-function-id.md)
- [Step 4: x86 to C Lifting](pipeline/04-lifting.md) — including jump tables the pattern match misses, and privileged registers
- [Step 5: Building the Runtime](pipeline/05-runtime.md) — manual overrides (replace, don't wrap), and `RECOMP_ABI_CHECK` wiring
- [Step 6: Iterative Debugging](pipeline/06-debugging.md) — including the ABI checker's standing reports, and guest registers in cdb

## Technical Deep Dives

### Recompiled code

- [The Register Model](technical/register-model.md) — Why global registers work and how the stack is simulated
- [Indirect Call Dispatch](technical/indirect-calls.md) — The RECOMP_ICALL problem and how to solve it
- [SEH and Exception Handling](technical/seh-handling.md) — Structured exception handling in recompiled code
- [Recovering a Split Function](technical/function-coalescence.md) — When the detector splits one function at an interior branch target: `--coalesce-functions`
- [MSVC RTTI Recovery](technical/rtti-recovery.md) — Class names, vtables and virtual methods from a title's RTTI, as seeds for disassembly
- [Conformance Testing](technical/conformance-testing.md) — Lifted C against the real CPU, the oracle that cannot be wrong

### Runtime

- [Memory Layout Reproduction](technical/memory-layout.md) — CreateFileMapping, mirror views, and address space tricks
- [Xbox Kernel Replacement](technical/kernel-replacement.md) — Mapping Xbox kernel ordinals to Win32, and the gotchas

### Graphics

- [D3D8 to D3D11 Translation](technical/d3d-translation.md) — Bridging Xbox's graphics API to modern DirectX
- [NV2A Shader Translation](technical/nv2a-shaders.md) — Register combiners and vertex microcode to HLSL
- [D3D8LTCG Device Context](technical/d3d8ltcg-device-context.md) — Per-image state recovery, queue contracts and hook ownership

### Reference analysis

- [Reference Analysis](technical/fusion-analysis.md) — Caller-supplied modules, packages, signature candidates and proof limits

### Project

- [Lessons Learned](technical/lessons-learned.md) — What worked, what didn't, mistakes to avoid
- [Gap Analysis vs xemu](technical/gap-analysis.md) — What's implemented, what's missing, prioritized roadmap
- [Target Evaluation](technical/candidate-games.md) — Executable requirements, runtime gaps and acceptance evidence
- [Runtime Integration](runtime/README.md) — Generic runtime interfaces and guest thread startup
- [xemu GDB Debugging](technical/xemu-debugging.md) — xemu's GDB stub as a reference debugger for runtime state

## Xbox Formats

- [XBE File Format](formats/xbe.md) — Xbox executable format reference
- [Xbox Kernel Exports](formats/kernel-exports.md) — Kernel functions, prototypes and Win32 equivalents
- [Xbox Disc Images](formats/disc-image.md) — Disc image formats and extracting game files

## Per-Module API Reference

- [Runtime Libraries](../src/README.md) — Architecture, build instructions, integration guide
- [xbox_kernel](../src/kernel/README.md) — Memory layout, file I/O, threading, sync, crypto, EEPROM, SMBus
- [xbox_d3d8](../src/d3d/README.md) — D3D8 interface, register combiners, vertex shaders, texture unswizzle
- [xbox_dsound](../src/audio/README.md) — DirectSound buffers, 3D audio, mixbins
- [xbox_apu](../src/apu/README.md) — MCPX APU voice processor, mixer, MMIO
- [xbox_nv2a](../src/nv2a/README.md) — NV2A GPU registers, push buffer, PGRAPH→D3D11
- [xbox_input](../src/input/README.md) — Gamepad state, vibration, button mapping

## Tools

- [Tools Reference](../tools/README.md) — Usage for every pipeline tool
- [Ghidra Naming](../tools/ghidra_naming/README.md) — Real names from Ghidra headless, and triage without the GUI
- [IDA Naming](../tools/ida_naming/README.md) — The same job through IDA
- [Debug Symbol Recovery](../tools/debug_symbols/README.md) — Which source file each function came from, from a debug build's assert strings
- [MSVC under Wine](../tools/conformance/msvc-wine/README.md) — The conformance suite's compiler on a non-Windows host

## By Symptom

Where to look when a title does something and says nothing about why.

| The title… | Look at |
|---|---|
| exits cleanly after two kernel calls | a thread start routine with no dispatch entry: `tools/seed_from_log`, [Functions the Detector Misses](pipeline/02-disassembly.md#functions-the-detector-misses) |
| logs `[ICALL] Failed to resolve VA` between two functions | [Reached only through a pointer](pipeline/02-disassembly.md#reached-only-through-a-pointer) |
| splits one function at an interior branch target | [Recovering a Split Function](technical/function-coalescence.md) |
| logs it for an address *inside* a function | [Back-to-back functions, merged](pipeline/02-disassembly.md#back-to-back-functions-merged), or a [jump table the lifter missed](pipeline/04-lifting.md#tables-the-pattern-match-misses) |
| starts no CRT thread at all | [A no-return call swallows the next function](pipeline/02-disassembly.md#a-no-return-call-swallows-the-next-function) |
| crashes on its first movie | [Library sections with no functions at all](pipeline/02-disassembly.md#library-sections-with-no-functions-at-all) |
| fails to build on `dr0`–`dr7` | [Privileged Registers](pipeline/04-lifting.md#privileged-registers) |
| ignores an override in `recomp_manual.c` | [Replace, Don't Wrap](pipeline/05-runtime.md#replace-dont-wrap), [Registers in recomp_manual.c](pipeline/05-runtime.md#registers-in-recomp_manualc) |
| duplicates `NtCurrentThread()` forever | [Pseudo-Handles Are Negative](technical/kernel-replacement.md#pseudo-handles-are-negative) |
| runs out of memory as the CRT heap grows | [The Heap Grows Only on 487](technical/kernel-replacement.md#the-heap-grows-only-on-487) |
| slows to 1 FPS some minutes into a level | [Counters Outlive 32 Bits](technical/kernel-replacement.md#counters-outlive-32-bits) |
| reports ABI violations in CRT helpers | [Reports RECOMP_ABI_CHECK Always Gives](pipeline/06-debugging.md#reports-recomp_abi_check-always-gives) |
