/**
 * The display side of the framebuffer window: what the CRTC is scanning and
 * what the PVIDEO overlay is showing. fb_present.c owns the window and these
 * facts; the D3D11 renderer (nv2a_d3d11.c) asks for them when it presents.
 */
#ifndef XBOXRECOMP_FB_PRESENT_H
#define XBOXRECOMP_FB_PRESENT_H

#include <stdint.h>

#if defined(_WIN32)
#include <windows.h>
/* The window to present into, or NULL until it exists. */
HWND xbox_FramebufferWindowHandle(void);
#endif

/* A buffer the display reads straight from guest memory because the title is
 * not flipping into it (loading screens drawn by the CPU, the first frames
 * before any flip). Returns 0 while flips are in charge. */
int xbox_FramebufferScanSource(uint32_t *va, uint32_t *pitch);

/* The PVIDEO overlay (movies), if one is showing. `yuy2` is a retained copy
 * of the submitted frame; `serial` changes whenever a new one is taken. The
 * s/t fields are the overlay's 12.20 fixed-point source walk. */
typedef struct {
    const uint8_t *yuy2;
    uint32_t pitch, in_w, in_h;
    uint32_t out_x, out_y, out_w, out_h;
    uint32_t start_s, start_t, ds_dx, dt_dy;
    uint32_t color_key_enabled, color_key;
    uint32_t serial;
} XboxOverlay;

int xbox_FramebufferOverlay(XboxOverlay *ovl);

/* Counts a presented frame for the title-bar FPS. */
void xbox_FramebufferNoteFlip(uint32_t draws);

#endif
