# xboxrecomp

```
 #   #  ####    ###   #   #         #####   ###    ###   #       ###
 #   #  #   #  #   #  #   #           #    #   #  #   #  #      #
  # #   ####   #   #   # #            #    #   #  #   #  #       ##
  # #   #   #  #   #   # #            #    #   #  #   #  #         #
 #   #  #   #  #   #  #   #           #    #   #  #   #  #         #
 #   #  ####    ###   #   #           #     ###    ###   #####   ###

 Static Recompilation Toolkit for Original Xbox Games
```

> Static x86-to-C translation and reusable Xbox runtime libraries. Ports supply game-specific integration and validation.

**[Join the sp00nznet recomp Discord](https://discord.gg/CRpzGWZFcu)** — the
community hub for sp00nznet's recomp projects, where ps3recomp development
happens in the open. Good place to ask questions, show a port you are working
on, or find out what people are stuck on before you duplicate the effort.

**Reusable pipeline and runtime.** The Python pipeline derives executable layout from the supplied XBE. Ports own guest-address overrides, authored assets and game policy; shared lifter/runtime fixes belong here. See [Target Evaluation](docs/technical/candidate-games.md) for the evidence needed to scope a port.

---

## What Is This?

This toolkit translates original Xbox x86 executables into C and builds reusable
runtime libraries for native ports. Function boundaries, calling conventions,
indirect targets and required runtime behavior need validation for each input.

Generated functions execute as native compiled code. Runtime libraries model
hardware and host services; their implementation coverage varies by path.
Ports own executable entry hooks, guest-address policy, assets and compatibility
data. See [Reference Analysis](docs/technical/fusion-analysis.md) for analysis
interfaces and [Target Evaluation](docs/technical/candidate-games.md) for scoping.

### Why Not Just Use an Emulator?

Emulators are great. Cxbx-Reloaded and xemu do incredible work. But static recomp offers some unique advantages:

- **Compiled guest functions** — ahead-of-time translation uses the native toolchain; measure performance on the port
- **Moddability** — the output is human-readable C code; you can patch, extend, and improve the game
- **Reusable interfaces** — shared C/runtime boundaries support host backends, with platform-specific validation
- **Separate artifacts** — ports keep generated code, authored changes and extracted assets distinct
- **Understanding** — the process forces you to deeply understand the game at the machine code level

## The Pipeline

```
         YOUR XBOX DISC
              |
              v
    +-------------------+
    |  1. Extract XBE   |     Extract default.xbe from the disc image
    +-------------------+
              |
              v
    +-------------------+
    |  2. Parse XBE     |     Read headers, sections, kernel imports
    +-------------------+     tools/xbe_parser/
              |
              v
    +-------------------+
    |  3. Disassemble   |     Find functions, build control flow graphs
    +-------------------+     tools/disasm/
              |
              v
    +-------------------+
    |  4. Identify      |     Classify: CRT, RenderWare, D3D, game code
    +-------------------+     tools/func_id/
              |
              v
    +-------------------+
    |  5. Lift to C     |     Translate x86 instructions to C statements
    +-------------------+     tools/recomp/
              |
              v
    +-------------------+
    |  6. Build Runtime  |    Kernel shim, D3D translation, memory layout
    +-------------------+     templates/runtime/
              |
              v
    +-------------------+
    |  7. Compile & Run  |    MSVC builds native .exe — game runs!
    +-------------------+
```

## Runtime Libraries

Following the [RexGlueSDK](https://github.com/rexglue/rexglue-sdk) pattern (which does the same for Xbox 360 via Xenia), xboxrecomp provides link-time libraries extracted from [xemu](https://github.com/xemu-project/xemu) and purpose-built compatibility layers. Your recompiled game links against these — no emulator needed at runtime.

| Targets | Role |
|---|---|
| `xbox_kernel` | Guest memory, import bridging and host-backed kernel services |
| `xbox_d3d8`, `xbox_nv2a` | Graphics API translation, GPU state and command execution |
| `xbox_video` | Presentation backends and video paths |
| `xbox_dsound`, `xbox_apu` | Mixer and MCPX audio paths |
| `xbox_usb`, `xbox_input` | USB/controller models and host input |
| `xbox_net` | Network device and transport paths |
| `xbox_host`, `platform` | Host bring-up and platform primitives |

Coverage is partial. Port acceptance requires runtime evidence for the paths
its executable uses. Third-party attribution is in [NOTICE](NOTICE).

### Building the Libraries

Use a native C toolchain and CMake 3.20+ (an MSVC developer terminal on Windows).
Choose a separate build directory per renderer:

```sh
# Windows: D3D11
cmake -S . -B build-d3d11 -DXBOXRECOMP_RENDERER=D3D11
cmake --build build-d3d11 --config Release

# Windows or Linux: SDL3 GPU
cmake -S . -B build-sdlgpu -DXBOXRECOMP_RENDERER=SDL3_GPU
cmake --build build-sdlgpu --config Release
```

SDL3 GPU configuration locates or fetches SDL3 and a DXC build with SPIR-V
support. Overrides and pinned dependencies are defined in
[cmake/Sdl3.cmake](cmake/Sdl3.cmake) and [cmake/Dxc.cmake](cmake/Dxc.cmake).
System dependencies remain platform-specific; Linux setup is under
[tools/linux/](tools/linux/).

Link the port against the `xboxrecomp` umbrella CMake target or selected library
targets. This project builds runtime libraries; the port supplies its executable.
The [new-game template](templates/new-game/) provides an integration starting
point. Its entry/lifecycle and all unresolved behavior require port validation.
The starter host entry is Windows-oriented; other hosts need lifecycle adaptation.

For an SDL3 GPU port executable, `RECOMP_SDL_GPU_DRIVER=vulkan` selects Vulkan;
`RECOMP_SDL_GPU_DRIVER=direct3d12` selects Direct3D 12 on Windows. These are
runtime choices within the SDL3 GPU build. The D3D11 build is selected separately
at CMake configuration time.

### Integration Pattern

Your recompiled game provides two callback functions that the kernel bridge calls to resolve function addresses:

```c
typedef void (*recomp_func_t)(void);
recomp_func_t recomp_lookup(uint32_t xbox_va);        // Auto-generated dispatch table
recomp_func_t recomp_lookup_manual(uint32_t xbox_va);  // Hand-written overrides
```

Generated sources provide dispatch; ports supply their manual overrides and
startup policy. Follow [runtime setup](docs/pipeline/05-runtime.md) for guest
memory/import initialization, owned stacks, hardware bring-up and shutdown.
Reported identity, launch input, image paths and authored behavior belong to
the port. Validate startup and representative gameplay against its own assets.

### Architecture

```text
Port executable
  generated functions + manual dispatch + lifecycle/assets/policy
  xboxrecomp umbrella
    kernel / guest memory / platform primitives
    D3D8 + NV2A -> D3D11 or SDL3 GPU
    audio / USB / input / network / video / host bring-up
```

## Quick Start

### Prerequisites

- **Windows** (D3D11 or SDL3 GPU) or **Linux** (SDL3 GPU; see `tools/linux/`)
- **Python 3.10+** with `capstone` (`pip install capstone`)
- A native C toolchain (MSVC on Windows)
- **CMake 3.20+**
- An original Xbox game disc image (you must own the game)

`py -3` below is the Windows Python Launcher — on Linux and macOS use
`python3`, and on a Microsoft Store install that has no `py`, use `python`.

### Step-by-Step

The condensed version. [docs/GETTING_STARTED.md](docs/GETTING_STARTED.md) is
the long one, and the one to read if a step here does not go as written — it
explains *why* each flag is there, which is what you need when your title
behaves differently from the example.

```bash
# 1. Clone this repo
git clone https://github.com/sp00nznet/xboxrecomp.git
cd xboxrecomp

# 2. Extract default.xbe from your Xbox disc image
#    (Use xdvdfs, extract-xiso, or similar tool)
mkdir game_files
# copy default.xbe and game data into game_files/

# 3. Parse the XBE — learn what you're working with
#    --json is NOT optional: step 4 reads the section layout back out of it.
#    The name matters too. Step 4 looks for <xbe stem>_analysis.json beside the
#    XBE, so keep it there and keep the suffix.
py -3 -m tools.xbe_parser game_files/default.xbe --json game_files/default_analysis.json
#    Output: section map, kernel imports, entry point, XDK version

# 4. Disassemble — find all functions
py -3 -m tools.disasm game_files/default.xbe --text-only
#    Output: tools/disasm/output/ (functions.json, xrefs.json, strings.json)
#    --text-only does what it says: only .text. A title with code in its XDK
#    library sections (D3D, DSOUND, XPP...) needs them named explicitly, e.g.
#    --extra-sections XIPS,DOLBY. Drop --text-only to take every code section.

# 5. Identify library functions
py -3 -m tools.func_id game_files/default.xbe -v
#    Output: tools/func_id/output/ (CRT, RenderWare, vtables classified)

# 6. Recover calling conventions and parameter counts
py -3 -m tools.abi_analysis game_files/default.xbe -v
#    Output: tools/abi_analysis/output/abi_functions.json
#    Skipping this still "works", but every function falls back to
#    cdecl / 0 params / int-or-void, so the generated signatures are guesses.

# 6b. Optional: real names instead of sub_XXXXXXXX, if you have Ghidra.
#     FidDb recognises the statically linked CRT/XDK helpers and names a few
#     hundred of them. Do it BEFORE step 8: the recompiler emits whatever name
#     is on the functions.json entry, so the names reach the generated C,
#     crash traces and ABI reports. See docs/GETTING_STARTED.md step 4.5.
XBE=game_files/default.xbe tools/ghidra_naming/run_ghidra.sh
py -3 tools/ghidra_naming/merge_names.py --apply

# 7. Create your game project — this is what becomes the .exe
#    The toolkit is a library; the executable lives in your own project.
cp -r templates/new-game ../mygame        # Windows cmd: xcopy /E /I templates\new-game ..\mygame
#    Then edit:
#      ../mygame/CMakeLists.txt  -> project name, XBOXRECOMP_DIR path
#      ../mygame/src/main.c      -> YOUR_GAME_ENTRY_POINT / XBE path from step 3

# 8. Lift to C — the big one
#    --gen-dir writes the generated code into your game project, where the
#    template's CMakeLists globs src/recomp/gen/*.c. Without it the output
#    lands in this repo (src/game/recomp/gen/) and nothing compiles it.
py -3 -m tools.recomp game_files/default.xbe --all --split 250 --gen-dir ../mygame/src/recomp/gen
#    Output: recomp_0000.c ... recomp_dispatch.c, recomp_funcs.h (millions of
#    lines of C), plus recomp_types.h — the runtime register model the
#    generated code includes. You do not supply that one; if the build says
#    "Cannot open include file: 'recomp_types.h'", this step did not finish.

# 9. Build and run — from the game project, not from xboxrecomp
cd ../mygame
cmake -S . -B build
cmake --build build --config Release
build\Release\your_game_recomp.exe          # named after project() in your CMakeLists
```

### What To Expect

Treat analysis and generation as an iterative bring-up process. Inspect missing
functions, ABI assumptions, indirect-call targets and runtime behavior before
adding overrides. Reusable instruction/runtime fixes belong in the toolkit;
application addresses, compatibility choices and assets belong in the port.

Use focused synthetic regressions for shared changes, then collect bounded
startup and gameplay evidence for the port. Keep unsupported paths explicit.

## Repository Structure

```
xboxrecomp/
├── README.md                    # You are here
├── CMakeLists.txt               # Top-level build (builds all runtime libs)
├── tools/                       # The recompilation toolchain (Python)
│   ├── xbe_parser/              # XBE file format parser
│   ├── disasm/                  # x86 disassembler + function detector
│   ├── func_id/                 # Library function identifier
│   ├── abi_analysis/            # Calling convention / param recovery
│   ├── recomp/                  # x86 -> C static recompiler
│   ├── debug_symbols/           # Debug-build symbol recovery
│   ├── symbols/ ghidra_naming/  # Optional symbol-name recovery (Ghidra)
│   ├── ida_naming/              # ... or the same thing through IDA
│   ├── xiso/ xmv/               # Disc image and video container tools
│   └── fusion/                  # MS Ficl/Fission study tooling
├── src/                         # Runtime libraries (C, link-time)
│   ├── kernel/                  # xbox_kernel - Xbox kernel → Win32
│   ├── d3d/                     # xbox_d3d8   - D3D8 → D3D11 graphics
│   ├── audio/                   # xbox_dsound - DirectSound compat
│   ├── apu/                     # xbox_apu    - MCPX APU emulation (xemu)
│   ├── nv2a/                    # xbox_nv2a   - NV2A GPU emulation (xemu)
│   ├── input/                   # xbox_input  - Gamepad → XInput
│   ├── video/                   # xbox_video  - FMV playback + framebuffer window
│   └── host/                    # xbox_host   - MMIO routing, window + device bring-up
├── include/xbox/                # Public umbrella header (xboxrecomp.h)
├── templates/                   # Starter templates for new projects
│   ├── new-game/                # ** Copy this to start a game project **
│   │   ├── CMakeLists.txt       # Builds the game .exe, links xboxrecomp
│   │   └── src/main.c           # Host entry point: loads XBE, boots guest
│   └── runtime/                 # Runtime shim templates
│       ├── recomp_types.h       # Register model + ICALL macros
│       ├── xbox_memory.h        # Includes runtime memory interface
│       └── kernel_stubs.h       # Includes runtime kernel interface
└── docs/                        # Documentation
    ├── pipeline/                # Step-by-step pipeline guides
    ├── technical/               # Deep technical documentation
    ├── formats/                 # Xbox file format references
    └── runtime/                 # Runtime implementation guides
```

## Documentation

### Start Here
- **[Getting Started Guide](docs/GETTING_STARTED.md)** — End-to-end walkthrough from XBE to running game
- **[Documentation Index](docs/INDEX.md)** — Every document in one place, and the same pages again by symptom
- **[Decompilation Guide](docs/DECOMP.md)** — Using this as a function splitter instead: one byte-exact `.s` per function, with signatures and the call graph. You never run the recompiler
- **[Tools Reference](tools/README.md)** — Detailed usage for every pipeline tool
- **[Runtime Libraries](src/README.md)** — Architecture, build instructions, integration guide

### Per-Module API Reference
- [xbox_kernel](src/kernel/README.md) — Memory layout, file I/O, threading, sync, crypto, EEPROM, SMBus (11,128 LOC)
- [xbox_d3d8](src/d3d/README.md) — D3D8 interface, register combiners, vertex shaders, texture unswizzle (8,838 LOC)
- [xbox_dsound](src/audio/README.md) — DirectSound buffers, 3D audio, mixbins (573 LOC)
- [xbox_apu](src/apu/README.md) — MCPX APU voice processor, mixer, MMIO (4,168 LOC)
- [xbox_nv2a](src/nv2a/README.md) — NV2A GPU registers, push buffer, PGRAPH→D3D11 (4,892 LOC)
- [xbox_input](src/input/README.md) — Gamepad state, vibration, button mapping (360 LOC)

### Pipeline Guides
- [Extracting and Parsing XBE Files](docs/pipeline/01-xbe-parsing.md)
- [Disassembly and Function Detection](docs/pipeline/02-disassembly.md)
- [Function Identification](docs/pipeline/03-function-id.md)
- [x86 to C Lifting](docs/pipeline/04-lifting.md)
- [Building the Runtime](docs/pipeline/05-runtime.md)
- [Iterative Debugging](docs/pipeline/06-debugging.md)

### Technical Deep Dives
- [The Register Model](docs/technical/register-model.md) — Why global registers work and how the stack is simulated
- [Memory Layout Reproduction](docs/technical/memory-layout.md) — CreateFileMapping, mirror views, and address space tricks
- [Indirect Call Dispatch](docs/technical/indirect-calls.md) — The RECOMP_ICALL problem and how to solve it
- [D3D8 to D3D11 Translation](docs/technical/d3d-translation.md) — Bridging Xbox's graphics API to modern DirectX
- [NV2A Shader Translation](docs/technical/nv2a-shaders.md) — Register combiners and vertex microcode to HLSL
- [D3D8LTCG Device Context](docs/technical/d3d8ltcg-device-context.md) — Per-image state recovery, queue contracts and hook ownership
- [Xbox Kernel Replacement](docs/technical/kernel-replacement.md) — Mapping Xbox kernel ordinals to Win32
- [SEH and Exception Handling](docs/technical/seh-handling.md) — Structured exception handling in recompiled code
- [Lessons Learned](docs/technical/lessons-learned.md) — What worked, what didn't, mistakes to avoid
- [Gap Analysis vs xemu](docs/technical/gap-analysis.md) — What's implemented, what's missing, prioritized roadmap
- [Reference Analysis](docs/technical/fusion-analysis.md) — Generic module, package, signature and comparison helpers
- [Runtime Integration](docs/runtime/README.md) — Generic runtime interfaces and guest thread startup

### Xbox Formats
- [XBE File Format](docs/formats/xbe.md) — Xbox executable format reference
- [Xbox Kernel Exports](docs/formats/kernel-exports.md) — All 366 kernel functions documented

## How It Works

The interesting parts each have their own document rather than a summary here,
so there is one place to keep correct:

- **[The Register Model](docs/technical/register-model.md)** — why the guest
  registers are globals (and thread-local), how the guest stack is simulated,
  and why every recompiled function is `void f(void)`.
- **[Memory Layout](docs/technical/memory-layout.md)** — image-derived mapping,
  owned guest storage, explicit aliases and quiesced shutdown.
- **[Indirect Call Dispatch](docs/technical/indirect-calls.md)** — `call [eax+0x10]`
  with no compile-time target. The hardest part of any bring-up.
- **[NV2A Shader Translation](docs/technical/nv2a-shaders.md)** — register
  combiners and vertex microcode to HLSL, both translated at runtime and cached.
- **[SEH and Exception Handling](docs/technical/seh-handling.md)** — how
  `__SEH_prolog`/`__SEH_epilog` are detected per title and bridged.

## Evaluating a Target

Inspect the executable, required runtime behavior and unresolved paths before
estimating a port. See [Target Evaluation](docs/technical/candidate-games.md)
for the evidence checklist and reusable-mechanism/port-policy boundary.

## How You Can Help

This is an emerging field. Here's how you can contribute:

1. **Try it on a new game** — Pick an Xbox exclusive, follow the pipeline, and see how far you get. Even partial results teach us about the toolchain's gaps.
2. **Improve the lifter** — Coverage is good but unquantified; the honest signal is that an unhandled instruction lifts to a bare `/* mnemonic */` comment, so grepping generated output for those finds the gaps. Segment prefixes and the rarer x87/SSE forms are where they cluster.
3. **Document Xbox formats** — Every game has its own asset formats. Document what you discover.
4. **Build runtime components** — Better D3D8 emulation, audio, networking — the runtime layer is where most per-game work happens.
5. **Share your findings** — Write up what you learn. The Xbox modding/preservation community benefits from every discovery.

Not sure where to start, or want to sanity-check an idea first? Ask in the
[Discord](https://discord.gg/CRpzGWZFcu) — several of the people working on
ports and on the lifter are there.

## Dependencies and checks

The core Python pipeline uses Python 3.10+ and Capstone. Python unit tests use
pytest; compiler/differential tests have additional toolchain requirements.
Naming integrations have their own optional dependencies. Runtime dependencies
are selected by CMake and the host backend.

```sh
python -m pytest tools/
python -m tools.conformance --help

# Example focused runtime suite (from the toolkit root)
cmake -S tests/memory_regressions -B build-memory
cmake --build build-memory --config Release
ctest --test-dir build-memory -C Release --output-on-failure
```

Other runtime suites live under [tests/](tests/). Choose the suites matching a
change and inspect what they exercise. Differential setup and platform-specific
oracle requirements are documented in
[Conformance Testing](docs/technical/conformance-testing.md).

## FAQ

**Q: Is this legal?**
A: This project provides tools and documentation. You must own a legitimate copy of any game you recompile. No copyrighted game code or assets are included in this repository.

**Q: How is this different from an emulator?**
A: Generated functions compile ahead of time. The runtime still models guest state, hardware and host services; those paths require their own validation.

**Q: Can I use this on Xbox 360 games?**
A: No. Xbox 360 uses PowerPC (big-endian, different ISA). See [XenonRecomp](https://github.com/hedge-dev/XenonRecomp) for Xbox 360 static recompilation. This toolkit is specifically for the original Xbox's x86 code.

**Q: How long does it take to get a game running?**
A: Estimate from recovered code/ABI coverage, unresolved indirect targets, required devices and observed runtime behavior. Measure startup and representative gameplay before estimating the remaining port work.

**Q: Why C output instead of direct x86-64 binary translation?**
A: C exposes translated state and control flow to ordinary compilers and debuggers. You can inspect the output, set breakpoints and change individual functions while validating the translation.

## License

**MIT** — see [LICENSE](LICENSE). Third-party components keep their original
licence:

| Component | Licence | Copyright |
|---|---|---|
| the MCPX APU sources in `src/apu/` | LGPL-2.1-or-later | espes; Jannik Vogel; Matt Borgerson |
| `src/nv2a/nv2a_regs.h` | LGPL-2.1-or-later | espes; Jannik Vogel |
| everything else | MIT | sp00nz and contributors |

The APU and the NV2A register definitions were extracted from
[xemu](https://github.com/xemu-project/xemu) and are that project's work, not
ours. LGPL-2.1 expressly permits linking them from MIT or proprietary code, so
a recompiled game is unaffected; what it asks is that the notices stay, the
source stays available, and users can relink against a modified library.
[LICENSES/LGPL-2.1.txt](LICENSES/LGPL-2.1.txt) is the verbatim licence text —
shipping it alongside those files is a requirement, not a courtesy.

Not every file under `src/apu/` and `src/nv2a/` is xemu-derived. See
[NOTICE](NOTICE) for the exact list, each with the copyright it actually
carries — including algorithms we implemented ourselves but learned from xemu,
credited there even where no licence obligation attaches.

## Contributors

xboxrecomp is built by more than one person. See
**[CONTRIBUTORS.md](CONTRIBUTORS.md)** for who did what — including the people
who never sent a patch and still moved the project further than a patch would
have, by finding the wall everyone else was about to hit.

Thank you, all of you.

## Credits

Built with [Claude Code](https://claude.ai) (Anthropic) — proving that AI-assisted systems programming can tackle problems previously considered impractical.

Human contributors are credited in [CONTRIBUTORS.md](CONTRIBUTORS.md); the
third-party code we build on is credited in [NOTICE](NOTICE).

## Changelog

Use `git log --oneline` for changes and contributors in the current checkout.
The test projects document scoped contracts. Validate each port against its own
executable/assets and the runtime paths it needs.

## References

- [XBE File Format](https://xboxdevwiki.net/Xbe) — Xbox Dev Wiki
- [Xbox Kernel Exports](https://xboxdevwiki.net/Kernel) — Xbox Dev Wiki
- [NV2A GPU](https://xboxdevwiki.net/NV2A) — Xbox GPU documentation
- [Xbox Architecture](https://www.copetti.org/writings/consoles/xbox/) — Copetti's deep dive
- [N64Recomp](https://github.com/N64Recomp/N64Recomp) — Static recomp for N64 (MIPS→C)
- [XenonRecomp](https://github.com/hedge-dev/XenonRecomp) — Static recomp for Xbox 360 (PPC→C)
- [RexGlueSDK](https://github.com/rexglue/rexglue-sdk) — Xbox 360 recomp runtime (Xenia as link-time library)
- [Cxbx-Reloaded](https://github.com/Cxbx-Reloaded/Cxbx-Reloaded) — Xbox emulator (dynamic recomp)
- [xemu](https://github.com/xemu-project/xemu) — Xbox emulator (LLE)
