# Host backends and hooks

A recompiled title links the runtime libraries and supplies the rest of the
host itself: its window, its GPU API, its audio device, its pads. Every
host-facing module has a function-level contract, and CMake can leave the
bundled implementation out so the embedding project provides its own (an SDL3
stack, for example) without carrying a fork of the toolkit.

## Replaceable backends

| Contract | Bundled implementation | Leave it out with | Header |
|---|---|---|---|
| NV2A renderer (`nv2a_d3d_*`) | `src/video/nv2a_d3d11.c` (D3D11) | `-DXBOXRECOMP_RENDERER=EXTERNAL` | `src/video/nv2a_d3d11.h` |
| Framebuffer window (`fbw_*`, `xbox_FramebufferWindowStart`) | `src/video/fb_window_win32.c` | `-DXBOXRECOMP_RENDERER=EXTERNAL` | `src/video/fb_window.h`, `fb_present.h` |
| APU audio device (`xa2_*`) | XAudio2 on Windows, SDL2 elsewhere | `-DXBOXRECOMP_APU_OUTPUT=EXTERNAL` | `src/apu/apu_xaudio2.h` |
| D3D8 layer (`xbox_d3d8`) | `src/d3d` | `-DXBOXRECOMP_BUILD_D3D8=OFF` | — |
| Host pads | XInput on Windows, none elsewhere | `xbox_InputSetHostPadProvider()` at run time | `src/input/xinput_xbox.h` |

The window and the renderer are one choice because the window's input and
overlay state is shared with the renderer's present path; a renderer that
brings its own window implements both contracts.

`tests/external_backends` is the checked list of what an embedder provides: it
builds the whole runtime with every `EXTERNAL` option on and links it against
`stub_backends.c`, which defines exactly those functions as no-ops and takes
their prototypes from the contract headers. `-DBUNDLED_BACKENDS=ON` links the
same program against the bundled backends, the default configuration's link
check.

## Hooks

Hooks are plain function pointers or small structs registered before the
subsystem starts, on the thread and under the rules their header comment states.

**Presentation and window** (`fb_present.h`, `video_player.h`)
`xbox_FramebufferSetCloseHandler`, `xbox_FramebufferSetCursorHandler`,
`xbox_FramebufferSetMousePressHandler`, `xbox_FramebufferSetHostOverlay`
(an ImGui/RmlUi layer drawn into the swapchain; handles are borrowed),
`xbox_FramebufferHostInputCaptured`, `xbox_VideoSetPresentHandler`.

**Renderer policy** (`nv2a_d3d11.h`)
`nv2a_d3d_set_overlay_hooks` (`NvOverlayHooks`: widescreen placement, vertex
sanitising, forced depth test, cubic filtering, idle present, vertex
alignment), `nv2a_pb_set_nop_hook`, `nv2a_d3d_host_draw` (ordered native
drawing on the current target), `nv2a_d3d_manual_present`,
`nv2a_d3d_repeat_frame`, `nv2a_d3d_interp_frame`.

**Input** (`xinput_xbox.h`)
`xbox_InputSetHostPadProvider`, `xbox_InputSetKeyboardHook`,
`xbox_InputSetMenuProbe`, `xbox_InputSetMenuClickHandler`.

**Kernel** (`kernel.h`, `nv2a_pb.h`, `icall_diagnostics.h`)
`xbox_kernel_set_bridge_override` (replace one kernel export),
`xbox_kernel_set_dvd_scsi_hook`, `xbox_kernel_set_inline_event_callers`,
`xbox_SetReadOnlyFileSharing`, `nv2a_pb_set_stop_handler`,
`xbox_icall_set_report_hook`.

**Guest access** (`guest_state.h`, `guest_read.h`, `guest_strings.h`,
`guest_call.h`, `xbox_std_string.h`)
Save and restore the guest register file and callee-saved set, bounds-checked
guest reads, guest string helpers, repeated guest calls.

## Writing an SDL3 (or any other) backend

Link your target next to `xboxrecomp` and define the functions in the table
above. The window module writes key and pointer state through
`fb_window.h` as events arrive; the renderer asks `fb_present.h` what the CRTC
and PVIDEO overlay are showing. Pads go through `XBOX_HOST_PAD_PROVIDER`:

```c
static DWORD my_get(DWORD port, XBOX_HOST_PAD *pad) { /* fill pad */ return ERROR_SUCCESS; }
static const XBOX_HOST_PAD_PROVIDER pads = { NULL, my_get, NULL };
xbox_InputSetHostPadProvider(&pads);   /* before xbox_InputInit() */
```

The provider supplies only the raw pad (XInput layout). The toolkit maps it to
the Xbox pad (`RECOMP_PADMAP`) and adds the keyboard and mouse, UI-capture
neutralisation and packet sequencing, so every backend behaves the same.
