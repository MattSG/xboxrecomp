/**
 * Between the display state (fb_present.c, every host) and the window that
 * shows it: fb_window_win32.c with the D3D11 renderer, fb_window_sdl.c with
 * the SDL3 GPU one. The window module writes the input state here as its
 * events arrive and reads the registered handlers.
 */
#ifndef XBOXRECOMP_FB_WINDOW_H
#define XBOXRECOMP_FB_WINDOW_H

#include <stddef.h>
#include <stdint.h>
#include "platform/xbox_winnt.h"
#include "fb_present.h"

/* Held keys and mouse buttons, by Windows virtual-key code (the codes
 * RECOMP_KEYMAP uses on every host). Written only by the window's thread and
 * read one byte at a time elsewhere: a press seen a frame late cannot be
 * told from one made a frame later. */
extern volatile unsigned char fbw_key_down[256];
/* The pointer as a fraction of the client area, and how often it moved. */
extern volatile float fbw_mouse_x, fbw_mouse_y;
extern volatile LONG fbw_mouse_moves;
/* Another window is in front (RECOMP_PAUSE_UNFOCUSED). */
extern volatile LONG fbw_inactive;

extern void (*fbw_close_handler)(void);
extern XboxCursor (*fbw_cursor_handler)(void);
extern int (*fbw_mouse_press_handler)(float x, float y);
extern XboxHostOverlay fbw_host_overlay;

/* The window module's native window (HWND or SDL_Window *), or NULL. */
void *fbw_native_window(void);

/* The title-bar text, "<title> | FPS: n | draws: n", once a second: returns
 * 1 and fills out (UTF-16, NUL-terminated) when it is due, 0 otherwise. */
int fbw_title_due(uint16_t *out, size_t count);

/* The initial client size: RECOMP_WINDOW_SIZE=WxH, else the display aspect
 * at about two thirds of the given work-area height. */
void fbw_initial_size(int work_w, int work_h, int *w, int *h);

#endif
