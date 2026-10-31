/**
 * The one place SDL events are pumped.
 *
 * The D3D8 device window and the framebuffer window both need events, from
 * different threads, and SDL's queue is shared: if each polled it they would
 * steal each other's key presses. Every caller goes through
 * xbox_PumpEvents(), which serialises polling and keeps the key state that
 * the RECOMP_KEYBOARD pad stand-in (src/input) reads.
 */
#include <SDL3/SDL.h>
#include <stdlib.h>
#include <string.h>

static SDL_Mutex     *g_mutex;
static SDL_InitState  g_init;
static unsigned char  g_keys[SDL_SCANCODE_COUNT];

static void ensure_mutex(void)
{
    if (SDL_ShouldInit(&g_init)) {
        g_mutex = SDL_CreateMutex();
        SDL_SetInitialized(&g_init, true);
    }
}

void xbox_PumpEvents(void)
{
    SDL_Event e;

    ensure_mutex();
    SDL_LockMutex(g_mutex);
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_EVENT_QUIT || e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
            SDL_UnlockMutex(g_mutex);
            exit(0);
        }
        if ((e.type == SDL_EVENT_KEY_DOWN || e.type == SDL_EVENT_KEY_UP) &&
            (unsigned)e.key.scancode < sizeof(g_keys)) {
            g_keys[e.key.scancode] = (e.type == SDL_EVENT_KEY_DOWN);
            /* RECOMP_KEY_TRACE: each key as it arrives, edge-triggered. A
             * sampled "which keys are held" cannot tell a key that was never
             * pressed from one that was tapped; this answers "did it arrive". */
            if (e.type == SDL_EVENT_KEY_DOWN && getenv("RECOMP_KEY_TRACE")) {
                static unsigned n;
                if (n++ < 40)
                    fprintf(stderr, "  [KEY] down scancode=%d\n", (int)e.key.scancode);
            }
        }
        /* Switching away with a key held would leave it held for ever. */
        if (e.type == SDL_EVENT_WINDOW_FOCUS_LOST)
            memset(g_keys, 0, sizeof(g_keys));
    }
    SDL_UnlockMutex(g_mutex);
}

/* Non-zero while the key with this Win32 virtual-key code is held in a
 * focused window. Always zero with no window, which is the right answer. */
int xbox_FramebufferKeyDown(int vk)
{
    SDL_Scancode sc = SDL_SCANCODE_UNKNOWN;

    if (vk >= 'A' && vk <= 'Z')      sc = (SDL_Scancode)(SDL_SCANCODE_A + (vk - 'A'));
    else if (vk >= '1' && vk <= '9') sc = (SDL_Scancode)(SDL_SCANCODE_1 + (vk - '1'));
    else switch (vk) {
    case 0x08: sc = SDL_SCANCODE_BACKSPACE; break;
    case 0x0D: sc = SDL_SCANCODE_RETURN; break;
    case 0x25: sc = SDL_SCANCODE_LEFT; break;
    case 0x26: sc = SDL_SCANCODE_UP; break;
    case 0x27: sc = SDL_SCANCODE_RIGHT; break;
    case 0x28: sc = SDL_SCANCODE_DOWN; break;
    case 0x62: sc = SDL_SCANCODE_KP_2; break;
    case 0x64: sc = SDL_SCANCODE_KP_4; break;
    case 0x66: sc = SDL_SCANCODE_KP_6; break;
    case 0x68: sc = SDL_SCANCODE_KP_8; break;
    case 0x10: return g_keys[SDL_SCANCODE_LSHIFT] || g_keys[SDL_SCANCODE_RSHIFT];
    case 0x11: return g_keys[SDL_SCANCODE_LCTRL] || g_keys[SDL_SCANCODE_RCTRL];
    default: return 0;
    }
    return sc != SDL_SCANCODE_UNKNOWN && g_keys[sc];
}
