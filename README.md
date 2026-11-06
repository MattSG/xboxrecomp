# xboxrecomp

A static recompilation toolkit for original Xbox executables. Python tools
parse an XBE, discover functions and translate x86 code to C. Your game project
compiles that output and links the runtime libraries; this repository alone
does not produce a playable game executable.

This fork builds on [sp00nznet/xboxrecomp](https://github.com/sp00nznet/xboxrecomp).
Keep title-specific hooks, assets and behavior in the game project.

## Runtime

SDL3 supplies host graphics, controllers, keyboard/mouse events and audio.
SDL GPU selects Direct3D 12, Vulkan or Metal as available on the host. FFmpeg
decodes video and WMA. The MCPX DSP interpreter/recompiler and native NV2A
pushbuffer executor are retained; the SDL3 migration does not replace guest
CPU or DSP behavior with media playback.

| Target | Purpose |
|---|---|
| `xboxrecomp` | Umbrella target for the runtime libraries |
| `xbox_kernel` | Guest memory, kernel calls, files, threads and synchronization |
| `xbox_d3d8` | Xbox D3D8 interface translated to SDL GPU |
| `xbox_nv2a` | NV2A registers and command processing |
| `xbox_apu` | MCPX voice processing, DSP and SDL audio output |
| `xbox_dsound` | DirectSound compatibility and software mixing |
| `xbox_input`, `xbox_usb` | Host input and guest controller discovery |
| `xbox_video` | FFmpeg playback and guest framebuffer presentation |

The active renderer is `SDL3_GPU`. Legacy D3D11 sources remain for comparison
fixtures; D3D11 is no longer a selectable production backend. A title may use
the D3D8 interface or execute its statically linked XDK through the native
NV2A path; these are separate paths and need separate validation.

## Build and integrate

Use Python 3.10+ with Capstone for the translation tools, CMake 3.20+, Git,
and a C/C++ compiler. On Windows, install Visual Studio 2022 or newer with
Desktop development with C++ and C++ CMake tools for Windows. Run CMake from
a developer shell.

```sh
cmake -S . -B build
cmake --build build --config Release --parallel
```

CMake fetches pinned SDL3, shader translation and DXC dependencies when needed.
Windows also downloads pinned FFmpeg shared libraries. Linux/macOS need
pkg-config and FFmpeg development libraries (`libavformat`, `libavcodec`,
`libavutil`, `libswscale`, `libswresample`), plus the platform dependencies for
SDL3 and OpenSSL. The full migration was validated on Windows; other hosts
require their own execution checks.

Start a title project from [templates/new-game](templates/new-game/), then
follow [Getting Started](docs/GETTING_STARTED.md) for extraction, analysis,
code generation and integration. Link `xboxrecomp` and call
`xbox_copy_runtime_dlls(your_executable)` after defining the executable so
Windows receives the required DXC and FFmpeg DLLs.

The generated dispatch table and title overrides provide
`recomp_lookup(uint32_t xbox_va)` and `recomp_lookup_manual(uint32_t xbox_va)`.
Owning a disc and producing C output does not establish that a title works:
validate its loading, rendering, input, audio and gameplay paths.

## Tests

From a configured compiler environment:

```sh
python -m pip install capstone pytest
python -m pytest tools/
python -m tools.conformance
```

Conformance compares lifted C with original x86 instructions. Its compiler
and oracle requirements are described in
[Conformance Testing](docs/technical/conformance-testing.md). Check skipped
tests as well as passes; a missing compiler can remove coverage.

Native fixtures live in `tests/`; configure the relevant fixture directory,
build it, then run CTest. Fixtures using the runtime can share downloaded
sources through CMake's FetchContent source overrides. GPU checks require a
working driver; passing them does not prove every title's pixels are correct.

## Documentation and history

- [Getting Started](docs/GETTING_STARTED.md)
- [Technical documentation](docs/technical/)
- [D3D8 on SDL GPU](src/d3d/README.md)
- [Upstream release history through v0.16.0](CHANGELOG.md)
- [Contributors](CONTRIBUTORS.md)
- [Community Discord](https://discord.gg/CRpzGWZFcu)

The changelog records historical releases, not the validation state of this fork.
Older guides may describe the former D3D11, SDL2/OpenGL or Media Foundation
backends; the build configuration and source define the current paths.

## License and assets

Toolkit code is [MIT](LICENSE) except where individual files specify otherwise.
xemu-derived APU sources and NV2A register definitions retain their LGPL notices;
FFmpeg and shader dependencies retain their own licenses. See [NOTICE](NOTICE)
and [LICENSES](LICENSES/) for attribution and license texts.

Do not commit disc images, game assets, generated game code, BIOS files, keys or
saves. Use your own lawfully obtained game files.
