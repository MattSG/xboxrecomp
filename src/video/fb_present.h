/**
 * The display side of the framebuffer window: what the CRTC is scanning and
 * what the PVIDEO overlay is showing. fb_present.c owns the window and these
 * facts; the D3D11 renderer (nv2a_d3d11.c) asks for them when it presents.
 */
#ifndef XBOXRECOMP_FB_PRESENT_H
#define XBOXRECOMP_FB_PRESENT_H

#include <stdint.h>

#if defined(XBOXRECOMP_SDL3)
/* SDL3 window (the SDL3 GPU renderer): an SDL cursor, SDL events. */
struct SDL_Window;
union SDL_Event;
typedef struct SDL_Cursor *XboxCursor;
/* The window to present into, or NULL until the renderer has opened it. */
struct SDL_Window *xbox_FramebufferSdlWindow(void);
/* 1 when Windows started the process hidden (STARTF_USESHOWWINDOW, SW_HIDE),
 * which SDL windows must honour themselves; 0 elsewhere. */
int xbox_FramebufferStartHidden(void);
/* SDL video hints every SDL window in the process should share (Linux: prefer
 * native Wayland); call before SDL's video subsystem starts. */
void xbox_FramebufferSdlHints(void);
#elif defined(_WIN32)
#include <windows.h>
typedef HCURSOR XboxCursor;
/* The window to present into, or NULL until it exists. */
HWND xbox_FramebufferWindowHandle(void);
#endif

#if defined(XBOXRECOMP_SDL3) || defined(_WIN32)
/* Register before window startup. Called on the window thread for client
 * cursor selection; NULL result hides it. Cursor ownership stays with caller. */
void xbox_FramebufferSetCursorHandler(XboxCursor (*handler)(void));
/* Window-thread press notification, before held-key sampling. Nonzero
 * consumes the left press; the callback must not call guest functions. */
void xbox_FramebufferSetMousePressHandler(int (*handler)(float x, float y));
/* Register before window startup. Called on its message thread when the user
 * closes the display; the title owns stopping its guest workers. */
void xbox_FramebufferSetCloseHandler(void (*handler)(void));

/* Optional host UI. Register before starting the window/render threads.
 * Message delivery and rendering can run on different threads; the client
 * owns synchronization. Device/context/target are borrowed: D3D11 device,
 * context and render-target view, or with SDL3 the SDL_GPUDevice, the
 * SDL_GPUCommandBuffer being recorded and the swapchain SDL_GPUTexture (the
 * overlay begins and ends its own render pass, loading what is there). */
typedef struct {
    void (*render)(void *window, void *device, void *context, void *target);
#if defined(XBOXRECOMP_SDL3)
    int (*event)(const union SDL_Event *event);
#else
    int (*message)(HWND window, UINT message, WPARAM wparam, LPARAM lparam);
#endif
    int (*capture)(void);
} XboxHostOverlay;
void xbox_FramebufferSetHostOverlay(const XboxHostOverlay *overlay);
void xbox_FramebufferRenderHostOverlay(void *device, void *context, void *target);
int xbox_FramebufferHostInputCaptured(void);
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
