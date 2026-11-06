/**
 * The pump behind xbox_VideoPlayFile().
 *
 * Owns a D3D8 device (and with it an SDL window) and a decoder, all on one
 * thread of its own. Nothing here reads or writes guest memory and nothing
 * runs on the guest's thread, so a title that is still booting keeps booting
 * while its video plays.
 *
 * The guest decides when. A title opens its movie file at the point its own
 * logic reaches the intro; the runtime notices that open and plays that file.
 */
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "video_player.h"
#include "d3d/d3d8_xbox.h"
#include "d3d/d3d8_internal.h"

static void (*s_present_handler)(unsigned frame);
void xbox_VideoSetPresentHandler(void (*handler)(unsigned frame))
{
    s_present_handler = handler;
}

#define VP_WINDOW_W 640
#define VP_WINDOW_H 480

static SDL_AtomicInt s_playing;
static char          s_path[1024];

static int SDLCALL vp_thread(void *unused)
{
    IDirect3D8 *d3d;
    IDirect3DDevice8 *dev = NULL;
    D3DPRESENT_PARAMETERS pp;
    HRESULT hr;
    Uint64 last;
    unsigned frames = 0;

    (void)unused;

    d3d = xbox_Direct3DCreate8(0);
    if (!d3d) {
        fprintf(stderr, "  [VIDEO] Direct3DCreate8 failed\n");
        goto done;
    }

    memset(&pp, 0, sizeof(pp));
    pp.BackBufferWidth        = VP_WINDOW_W;
    pp.BackBufferHeight       = VP_WINDOW_H;
    pp.BackBufferFormat       = D3DFMT_X8R8G8B8;
    pp.BackBufferCount        = 1;
    pp.SwapEffect             = D3DSWAPEFFECT_DISCARD;
    pp.Windowed               = TRUE;
    pp.EnableAutoDepthStencil = TRUE;
    pp.AutoDepthStencilFormat = D3DFMT_D24S8;

    /* The device creates its own SDL window; no window handle is passed. */
    hr = d3d->lpVtbl->CreateDevice(d3d, 0, 0, NULL, 0, &pp, &dev);
    if (FAILED(hr) || !dev) {
        fprintf(stderr, "  [VIDEO] CreateDevice failed: 0x%08lX\n", (unsigned long)hr);
        goto done;
    }

    if (video_init() != 0)
        goto done;
    if (video_open(s_path) != 0) {
        fprintf(stderr, "  [VIDEO] could not open %s\n", s_path);
        goto done;
    }

    fprintf(stderr, "  [VIDEO] playing %s\n", s_path);
    last = SDL_GetTicks();

    while (SDL_GetAtomicInt(&s_playing)) {
        Uint64 now = SDL_GetTicks();
        float dt = (float)(now - last) / 1000.0f;
        last = now;

        /* video_update returns 1 when it uploaded a new frame, 0 when the
         * next one is not due yet, and negative at end of stream or on error.
         * Only the negative case ends playback -- treating any non-zero as
         * failure stops on the first frame that actually decoded, which is
         * every frame worth showing. */
        if (video_update(dt) < 0 || video_is_finished())
            break;

        /* Clear, then the frame as a fullscreen quad, then present (which
         * also pumps the window's events). */
        dev->lpVtbl->Clear(dev, 0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER,
                           0xFF000000u, 1.0f, 0);
        dev->lpVtbl->BeginScene(dev);
        video_render();
        dev->lpVtbl->EndScene(dev);
        dev->lpVtbl->Present(dev, NULL, NULL, NULL, NULL);
        frames++;

        if (s_present_handler) s_present_handler(frames);

        SDL_Delay(1);
    }

    fprintf(stderr, "  [VIDEO] finished %s after %u presented frames\n",
            s_path, frames);
    video_close();
    video_shutdown();

done:
    SDL_SetAtomicInt(&s_playing, 0);
    return 0;
}

int xbox_VideoPlayFile(const char *host_path)
{
    SDL_Thread *th;

    if (!host_path || !*host_path)
        return -1;
    /* One at a time: the pump owns a device and a window, and a second one
     * would fight the first for both. */
    if (!SDL_CompareAndSwapAtomicInt(&s_playing, 0, 1))
        return -1;

    SDL_strlcpy(s_path, host_path, sizeof(s_path));

    th = SDL_CreateThread(vp_thread, "video pump", NULL);
    if (!th) {
        SDL_SetAtomicInt(&s_playing, 0);
        return -1;
    }
    SDL_DetachThread(th);
    return 0;
}

int xbox_VideoIsPlaying(void)
{
    return SDL_GetAtomicInt(&s_playing) != 0;
}
