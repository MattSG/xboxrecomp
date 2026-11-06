# What the MattSG fork carries on top of upstream

Measured against upstream `343b6d89` (v0.17.0): the fork's `main` is 97
commits and 496 files ahead (about +79k / -19k lines). Its history is one
consolidated delta, so the inventory is by path, not by commit. This branch
(`hooks/host-extension-points`) takes the part that is not tied to a specific
host API and turns the rest into replaceable backends, so a game repository can
keep its SDL3 stack itself and depend on upstream.

## Taken onto this branch unchanged from the fork

| Area | Paths | What it is |
|---|---|---|
| Kernel / runtime | `src/kernel`, `src/platform`, `include` | Physical-address model, dispatch lock/DPC/timers, file sharing and I/O completion, EEPROM and System Link crypto, guest-state / guest-read / guest-string helpers, icall + crash diagnostics, bridge overrides, POSIX Win32 layer |
| NV2A | `src/kernel/nv2a_*`, `src/nv2a` | Pushbuffer scanner and executor (`nv2a_pb_exec`), vertex-program interpreter, combiners, MMIO hook, WBC flush, recovery handler |
| APU | `src/apu` (minus `apu_sdl.c`) | DSP56300 core for the GP/EP (from xemu, see `dsp/PROVENANCE.md`), voice-processor fixes, `XBOXRECOMP_DSP_PROGRAM_DIR` recompilation |
| USB / net | `src/usb`, `src/net` | OHCI on every x86-64 host; NVNet over a host socket backend |
| Host bring-up | `src/host`, `templates/new-game` | One startup path for titles and the starter |
| Video (neutral) | `src/video` (`fb_present`, `video_player`, `spike_sampler`, `nv2a_d3d11.c`, `fb_window_win32.c`, headers) | Display state, PVIDEO scanout, FMV player on FFmpeg, D3D11 renderer |
| Tools | `tools/*` | Lifter fixes, `func_id`, `disasm`, `bench`, `launch`, `rtti`, `xiso` hardening |
| Dead code | `src/nv2a/menu_pb_*`, `nv2a_pgraph_d3d11.*`, `nv2a_pb_replay.c` | Removed prototypes |
| Tests | `tests/*` (all that do not need SDL) | Kernel, NV2A, APU, DSP, platform, net regression tests |

## Added on this branch (new, not in the fork)

- `XBOXRECOMP_RENDERER` (`D3D11` | `EXTERNAL`) and `XBOXRECOMP_APU_OUTPUT`
  (`DEFAULT` | `EXTERNAL`): leave the bundled implementation out.
- `xbox_InputSetHostPadProvider`: host pads from the embedder, replacing the
  XInput / SDL2 backend. The fork's `xbox_InputSetKeyboardHook`,
  `xbox_InputSetMenuProbe` and `xbox_InputSetMenuClickHandler` are carried over
  on top of upstream's input layer.
- `nv2a_d3d_flip_present` and `xbox_FramebufferWindowStart` promoted into the
  renderer / window contract headers; `tests/external_backends` checks both
  contracts by linking the whole runtime against stubs.
- `docs/technical/host-backends.md`.

## Left out: what moves into the game repository

These are the fork's concrete SDL3 implementations. Each implements a contract
above and has no other users.

| Fork path | Implements |
|---|---|
| `src/video/nv2a_sdlgpu.c`, `src/video/shaders/sdlgpu/*`, `cmake/Nv2aSdlgpu.cmake` | `nv2a_d3d_*` on SDL_GPU |
| `src/video/fb_window_sdl.[ch]`, `src/video/fb_present_sdl.c`, `src/gfx/*` | `fbw_*` window, event pump, SDL_GPU device helpers |
| `src/input/xinput_device.c` (SDL3 pad path) | the host pad provider |
| `src/apu/apu_sdl.c` | `xa2_*` on an SDL3 audio stream |
| `src/d3d/*` (SDL_GPU port), `src/d3d/d3d8_clear.c`, `d3d8_pipeline.c` | the `xbox_d3d8` target |
| `cmake/Sdl3.cmake`, `cmake/Dxc.cmake`, `cmake/xbox_sdl3.cmake`, `cmake/sdl3-d3d12-descriptors.patch` | fetching SDL3, SDL_shadercross, DXC |
| `tests/{gpu_*,video_backends,video_pump,input_sdl_*,apu_sdl,cmake_sdlgpu,nv2a_{bindings,samplers,textures,targets,readback,...}}` | tests of the above |

## Left out: not host-API specific, still to upstream

- `README.md` / `CHANGELOG.md` rewrites and `NOTICE` additions (DSP, FFmpeg).
- Fork edits to upstream's `src/d3d` that only matter once the d3d8 layer is the
  SDL_GPU one.

## Behaviour change against upstream

Upstream's POSIX build presents through an SDL2 window (`fb_present_sdl.c`) and
its SDL2 pad and audio code. This branch drops the SDL2 presenter, because the
fork's `fb_present.c` is split from its window; off Windows
`XBOXRECOMP_RENDERER` therefore defaults to `EXTERNAL` and the embedder
supplies the window and renderer. Windows behaviour is unchanged apart from the
D3D11 renderer replacing the Win32 GDI presenter's role for NV2A output.

## Tests

`tests/` holds the fork's tests that do not need SDL, built and run one project
at a time on Windows (MSVC, Release; ffmpeg's DLLs on `PATH`). 68 of the 80 that
build on Windows passed the sweep; the rest of that sweep was fixed or dropped:

- Fixed: `kernel_dvd_scsi` (configure the kernel data region first, like the
  fork's other kernel tests).
- Dropped because they already fail against the fork's own runtime, and are to
  be rewritten against the new code before going upstream:
  `apu_vp` (mono HRTF routing check), `apu_monitor`, `nv2a_raster_batch`
  (the software rasteriser path is gone from the fork's executor),
  `nv2a_pb_constants`, `nv2a_tex_filter`, the `nv2a-pb-hitch` / `nv2a-pb-capture`
  targets of `kernel_dispatch`, `fb_present`, `d3d8_smoke`.
- Dropped because they exercise the SDL3 pad path that stays with the embedder:
  `input_capture`, `input_usb_report`, `input_keyboard`, `input_keyboard_map`,
  `input_keyboard_sdl`, `input_sdl_hotplug`, `input_xinput_imports`.
- POSIX-only projects (`*_posix`, `kernel_fd_limit`, `platform_*`,
  `usb_ohci_posix`, `memory_layout_posix`) do not configure on Windows and were
  not run.

`tools/` pytest: the same tests fail as on upstream (a read-only `%TEMP%`,
`unicorn` missing); the 6 extra in this tree were environmental too, and
`tools/recomp` passes 727 with a proper environment.
