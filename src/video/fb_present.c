/**
 * The framebuffer display: what the title's display shows, on the host.
 *
 * The title renders through the NV2A and tells the kernel where its display
 * buffer is through AvSetDisplayMode; on hardware the CRTC scans that memory
 * out and the PVIDEO overlay composites movies over it. This file keeps track
 * of what the display is showing -- flips, buffers the title draws with the
 * CPU instead of flipping, and PVIDEO submissions -- and holds the input the
 * window reports (keys, pointer, focus).
 *
 * The window itself is fb_window_win32.c (with the D3D11 renderer) or
 * fb_window_sdl.c (with the SDL3 GPU renderer); the pixels are put on screen
 * by the renderer, which asks this file for the scan-out and overlay state
 * when it presents.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include "fb_window.h"
#include "../kernel/xbox_memory_layout.h"
#include "../nv2a/nv2a_regs.h"

extern ptrdiff_t xbox_GetMemoryOffset(void);
extern bool nv2a_hook_pvideo_snapshot(uint32_t regs[0x1000 / 4]);
extern void nv2a_hook_pvideo_consume(unsigned bank);

void (*fbw_close_handler)(void);
XboxHostOverlay fbw_host_overlay;
XboxCursor (*fbw_cursor_handler)(void);
int (*fbw_mouse_press_handler)(float, float);

void xbox_FramebufferSetMousePressHandler(int (*handler)(float, float))
{
    fbw_mouse_press_handler = handler;
}

void xbox_FramebufferSetCursorHandler(XboxCursor (*handler)(void))
{
    fbw_cursor_handler = handler;
}

void xbox_FramebufferSetHostOverlay(const XboxHostOverlay *overlay)
{
    if (overlay) fbw_host_overlay = *overlay;
}

void xbox_FramebufferRenderHostOverlay(void *device, void *context, void *target)
{
    if (fbw_host_overlay.render)
        fbw_host_overlay.render(fbw_native_window(), device, context, target);
}

int xbox_FramebufferHostInputCaptured(void)
{
    return fbw_host_overlay.capture && fbw_host_overlay.capture();
}

void xbox_FramebufferSetCloseHandler(void (*handler)(void))
{
    fbw_close_handler = handler;
}

static uint32_t      s_fb_va, s_fb_pitch;
/* Set by the first flip: from then on flips decide what is shown, except for a
 * buffer the title points the display at and draws into with the CPU. */
static volatile LONG s_flipped;

/* A buffer the title pointed the display at itself, read live until it flips
 * again.
 *
 * A flip is not the only way a picture changes. Between flips a title can set
 * the display mode on a buffer and draw into it with the CPU. Nothing rasterises into a buffer the title is showing this
 * way, so reading it live is what the CRTC would scan. */
static volatile LONG s_scan_va;            /* 0 while flips are in charge */
static uint32_t      s_scan_pitch;

/* A consumed submission remains on screen until the next submission or STOP.
 * Keep its pixels, since consuming the bank lets the guest reuse its memory. */
static uint8_t      *s_pvideo_source;
static size_t        s_pvideo_bytes;
static unsigned      s_pvideo_bank;
static uint32_t      s_pvideo_regs[0x1000 / 4];
static uint32_t      s_pvideo_serial;
static volatile LONG s_pvideo_capture_count;
static DWORD         s_pvideo_next_capture_tick;

/* Optional per-submission thumbnails for detecting corruption between
 * screenshots: each record is a 160x120 BGRA image of the submitted frame.
 * Keeping this disabled has no file IO; the capture never modifies guest
 * memory. */
static void pvideo_capture_submission(const uint8_t *yuy2, unsigned pitch,
                                      unsigned iw, unsigned ih)
{
    static FILE *capture;
    static int initialized;
    uint32_t row[160];
    unsigned x, y;
    if (!initialized) {
        const char *path = getenv("RECOMP_PVIDEO_FRAME_AUDIT");
        initialized = 1;
        if (path && *path) {
            capture = fopen(path, "wb");
            if (!capture)
                fprintf(stderr, "[PVIDEO_AUDIT] cannot open %s\n", path);
            else
                setvbuf(capture, NULL, _IOFBF, 256 * 1024);
        }
    }
    if (!capture || !iw || !ih) return;
    for (y = 0; y < 120; ++y) {
        const uint8_t *line = yuy2 + (size_t)(y * ih / 120) * pitch;
        for (x = 0; x < 160; ++x) {
            unsigned sx = x * iw / 160;
            int c = line[(sx & ~1u) * 2 + (sx & 1u) * 2] - 16;
            int l = c < 0 ? 0 : (c > 219 ? 255 : c * 255 / 219);
            row[x] = 0xFF000000u | (uint32_t)l * 0x010101u;
        }
        if (fwrite(row, sizeof row, 1, capture) != 1) {
            fclose(capture);
            capture = NULL;
            fprintf(stderr, "[PVIDEO_AUDIT] capture write failed\n");
            return;
        }
    }
    fflush(capture);
}

/* Take any newly submitted PVIDEO bank: copy its pixels, remember its
 * registers, and hand the bank back to the title. */
static void pvideo_poll(void)
{
    uint32_t regs[0x1000 / 4];
    unsigned bank;
    if (!nv2a_hook_pvideo_snapshot(regs)) return;
    {
        static int trace = -1;
        static uint32_t previous[7];
        uint32_t current[7] = {
            regs[NV_PVIDEO_STOP / 4],
            regs[NV_PVIDEO_BASE / 4], regs[(NV_PVIDEO_BASE + 4) / 4],
            regs[NV_PVIDEO_OFFSET / 4], regs[(NV_PVIDEO_OFFSET + 4) / 4],
            regs[NV_PVIDEO_SIZE_IN / 4], regs[(NV_PVIDEO_SIZE_IN + 4) / 4],
        };
        if (trace < 0) trace = getenv("RECOMP_PVIDEO_TRACE") != NULL;
        if (trace && memcmp(previous, current, sizeof current)) {
            fprintf(stderr, "[PVIDEO_SCANOUT] pending=%08X stop=%08X "
                    "size=%08X/%08X offset=%08X/%08X retained=%u\n",
                    regs[NV_PVIDEO_BUFFER / 4], current[0], current[5],
                    current[6], current[3], current[4],
                    s_pvideo_source != NULL);
            memcpy(previous, current, sizeof current);
        }
    }
    if ((regs[NV_PVIDEO_STOP / 4] & 1u) ||
        (s_pvideo_source &&
         regs[(NV_PVIDEO_SIZE_IN + s_pvideo_bank * 4) / 4] == UINT32_MAX)) {
        free(s_pvideo_source);
        s_pvideo_source = NULL;
        if (regs[NV_PVIDEO_STOP / 4] & 1u) return;
    }
    for (bank = 0; bank < 2; ++bank) {
        uint64_t physical = (uint64_t)regs[(NV_PVIDEO_BASE + bank * 4) / 4] +
                            regs[(NV_PVIDEO_OFFSET + bank * 4) / 4];
        unsigned pitch = regs[(NV_PVIDEO_FORMAT + bank * 4) / 4] & 0x1fff;
        unsigned height = (regs[(NV_PVIDEO_SIZE_IN + bank * 4) / 4] >> 16) & 0x7ff;
        uint64_t bytes = (uint64_t)pitch * height;
        uintptr_t va;
        MEMORY_BASIC_INFORMATION mapping;
        uint8_t *snapshot;
        size_t available;
        if (!(regs[NV_PVIDEO_BUFFER / 4] & (1u << (bank * 4u)))) continue;
        if (!bytes) continue;
        if (physical + bytes > g_xbox_total_ram) {
            /* Installed RAM bounds scanout, independently of extra guest backing. */
            static int range_logged;
            if (!range_logged) {
                range_logged = 1;
                fprintf(stderr, "[PVIDEO_RANGE] bank=%u base=%08X offset=%08X pitch=%u height=%u physical=%llX outside installed RAM, skipped\n",
                        bank, regs[(NV_PVIDEO_BASE + bank * 4) / 4],
                        regs[(NV_PVIDEO_OFFSET + bank * 4) / 4], pitch, height,
                        (unsigned long long)physical);
                fflush(stderr);
            }
            continue;
        }
        va = (uintptr_t)(0x80000000u + (uint32_t)physical) +
             (uintptr_t)xbox_GetMemoryOffset();
        if (!VirtualQuery((const void *)va, &mapping, sizeof(mapping)) ||
            mapping.State != MEM_COMMIT || (mapping.Protect & (PAGE_GUARD | PAGE_NOACCESS)))
            continue;
        available = mapping.RegionSize - (size_t)(va - (uintptr_t)mapping.BaseAddress);
        if (bytes > available) continue;
        snapshot = (uint8_t *)malloc((size_t)bytes);
        if (!snapshot) continue;
        memcpy(snapshot, (const void *)va, (size_t)bytes);
        {
            const char *capture = getenv("RECOMP_PVIDEO_DUMP");
            DWORD now = GetTickCount();
            if (capture && (LONG)(now - s_pvideo_next_capture_tick) >= 0) {
                LONG frame = InterlockedIncrement(&s_pvideo_capture_count);
                s_pvideo_next_capture_tick = now + 500;
                if (frame <= 12) {
                    char path[1024];
                    FILE *f;
                    int n = snprintf(path, sizeof(path), "%s.frame%02ld.bank%u.regs.bin",
                                     capture, (long)frame, bank);
                    f = n > 0 && (size_t)n < sizeof(path) ? fopen(path, "wb") : NULL;
                    if (f) {
                        fwrite(regs, 1, sizeof(regs), f);
                        fclose(f);
                    }
                    n = snprintf(path, sizeof(path), "%s.frame%02ld.bank%u.yuy2",
                                 capture, (long)frame, bank);
                    f = n > 0 && (size_t)n < sizeof(path) ? fopen(path, "wb") : NULL;
                    if (f) {
                        fwrite(snapshot, 1, (size_t)bytes, f);
                        fclose(f);
                    }
                    fprintf(stderr,
                            "  [PVIDEO] captured frame=%ld bank=%u physical=0x%llX format=0x%08X pitch=%u size=%ux%u bytes=%llu\n",
                            (long)frame, bank, (unsigned long long)physical,
                            regs[(NV_PVIDEO_FORMAT + bank * 4) / 4], pitch,
                            regs[(NV_PVIDEO_SIZE_IN + bank * 4) / 4] & 0x7ff,
                            height, (unsigned long long)bytes);
                }
            }
        }
        free(s_pvideo_source);
        s_pvideo_source = snapshot;
        s_pvideo_bytes = (size_t)bytes;
        s_pvideo_bank = bank;
        memcpy(s_pvideo_regs, regs, sizeof(s_pvideo_regs));
        s_pvideo_serial++;
        pvideo_capture_submission(snapshot, pitch,
                                  regs[(NV_PVIDEO_SIZE_IN + bank * 4) / 4] & 0x7ff,
                                  height);
        nv2a_hook_pvideo_consume(bank);
    }
}

int xbox_FramebufferOverlay(XboxOverlay *o)
{
    const uint32_t *regs = s_pvideo_regs;
    unsigned bank;
    uint32_t format;
    pvideo_poll();
    if (!s_pvideo_source)
        return 0;
    bank = s_pvideo_bank;
#define PV(reg) regs[((reg) + bank * 4u) / 4u]
    format = PV(NV_PVIDEO_FORMAT);
    memset(o, 0, sizeof *o);
    o->yuy2 = s_pvideo_source;
    o->pitch = format & NV_PVIDEO_FORMAT_PITCH;
    o->in_w = PV(NV_PVIDEO_SIZE_IN) & 0x7ff;
    o->in_h = (PV(NV_PVIDEO_SIZE_IN) >> 16) & 0x7ff;
    o->out_x = PV(NV_PVIDEO_POINT_OUT) & 0xfff;
    o->out_y = (PV(NV_PVIDEO_POINT_OUT) >> 16) & 0xfff;
    o->out_w = PV(NV_PVIDEO_SIZE_OUT) & 0xfff;
    o->out_h = (PV(NV_PVIDEO_SIZE_OUT) >> 16) & 0xfff;
    o->start_s = (PV(NV_PVIDEO_POINT_IN) & 0x7fff) << 16;
    o->start_t = (PV(NV_PVIDEO_POINT_IN) >> 17) << 17;
    o->ds_dx = PV(NV_PVIDEO_DS_DX);
    o->dt_dy = PV(NV_PVIDEO_DT_DY);
    o->color_key_enabled = (format & NV_PVIDEO_FORMAT_DISPLAY) != 0;
    o->color_key = regs[NV_PVIDEO_COLOR_KEY / 4] & 0xffffffu;
    o->serial = s_pvideo_serial;
#undef PV
    if (!o->in_w || !o->in_h || !o->out_w || !o->out_h || (o->in_w & 1u) ||
        o->pitch < o->in_w * 2u ||
        ((format & NV_PVIDEO_FORMAT_COLOR) >> 16) != 1u ||
        (uint64_t)o->pitch * o->in_h > s_pvideo_bytes)
        return 0;
    return 1;
}

void xbox_FramebufferWindowSet(uint32_t fb_va, uint32_t pitch)
{
    /* RECOMP_FB_VA pins the display to one guest address instead of following
     * whichever surface is being drawn into. */
    const char *pin = getenv("RECOMP_FB_VA");

    s_fb_va = pin ? (uint32_t)strtoul(pin, NULL, 0) : fb_va;
    if (pitch)
        s_fb_pitch = pitch;
}

/* Called by the pushbuffer executor when the title flips. */
void xbox_FramebufferWindowPresent(uint32_t fb_va, uint32_t pitch)
{
    (void)pitch;
    InterlockedExchange(&s_flipped, 1);
    /* Presenting the buffer already on display hands nothing over: D3D
     * persisting its display flips the very buffer it just set the mode on,
     * and the title then keeps drawing into it with no further flips. */
    if ((uint32_t)s_scan_va != fb_va)
        InterlockedExchange(&s_scan_va, 0);
}

/* The CPU executor's anti-aliased surfaces (nv2a_pb_exec.c). The renderers
 * present their own surfaces, so there is nothing to scale here. */
void xbox_FramebufferWindowSetAA(uint32_t sy) { (void)sy; }

/* Called from AvSetDisplayMode. Before the first flip the display already
 * reads guest memory live; this only matters once flips have taken over. */
void xbox_FramebufferWindowScanout(uint32_t fb_va, uint32_t pitch)
{
    if (!s_flipped || !fb_va || !pitch)
        return;
    s_scan_pitch = pitch;
    InterlockedExchange(&s_scan_va, (LONG)fb_va);
}

int xbox_FramebufferScanSource(uint32_t *va, uint32_t *pitch)
{
    static int pinned = -1;
    if (pinned < 0) pinned = getenv("RECOMP_FB_VA") != NULL;
    if (pinned && s_fb_va) {
        *va = s_fb_va;
        *pitch = s_fb_pitch;
        return 1;
    }
    if (s_scan_va) {
        *va = (uint32_t)s_scan_va;
        *pitch = s_scan_pitch;
        return 1;
    }
    if (!s_flipped && s_fb_va && s_fb_pitch) {
        *va = s_fb_va;
        *pitch = s_fb_pitch;
        return 1;
    }
    return 0;
}

/* Title bar, the way ps3recomp's window shows it. Written by the flip,
 * read once a second by the window thread; a torn read shows one stale
 * number for a second, which nobody can tell apart from a real one. */
static uint16_t      s_title[48] = { 'X', 'b', 'o', 'x', ' ', 'R', 'e', 'c', 'o', 'm', 'p', 0 };
static volatile LONG s_flips;
static volatile LONG s_frame_draws;

void xbox_FramebufferWindowSetTitle(const uint16_t *name, int max_chars)
{
    int i;
    for (i = 0; i < max_chars && i < 47 && name[i]; i++)
        s_title[i] = name[i];
    s_title[i] = 0;
}

void xbox_FramebufferNoteFlip(uint32_t draws)
{
    InterlockedIncrement(&s_flips);
    InterlockedExchange(&s_frame_draws, (LONG)draws);
}

void xbox_FramebufferWindowFrameStats(uint32_t draws)
{
    xbox_FramebufferNoteFlip(draws);
}

int fbw_title_due(uint16_t *out, size_t count)
{
    static DWORD t0;
    static LONG f0;
    DWORD now = GetTickCount();
    LONG f = s_flips;
    char stats[80];
    size_t n = 0, k;
    if (now - t0 < 1000 || !count) return 0;
    snprintf(stats, sizeof stats, " | FPS: %.1f | draws: %ld",
             t0 ? (f - f0) * 1000.0 / (now - t0) : 0.0, (long)s_frame_draws);
    t0 = now; f0 = f;
    for (k = 0; s_title[k] && n + 1 < count; k++) out[n++] = s_title[k];
    for (k = 0; stats[k] && n + 1 < count; k++) out[n++] = (uint16_t)(unsigned char)stats[k];
    out[n] = 0;
    return 1;
}

/* RECOMP_PAUSE_UNFOCUSED: while another window is in front the executor
 * holds the next vblank (the title waits on the GPU, so it stops) and the
 * audio goes quiet. */
volatile LONG fbw_inactive;

int xbox_FramebufferPaused(void)
{
    static int on = -1;
    if (on < 0)
        on = getenv("RECOMP_PAUSE_UNFOCUSED") && atoi(getenv("RECOMP_PAUSE_UNFOCUSED")) != 0;
    return on && fbw_inactive;
}


volatile float fbw_mouse_x, fbw_mouse_y;
volatile LONG fbw_mouse_moves;

/* The pointer as a fraction of the client area (the window's DPI and the
 * swap chain's pixels need not agree); returns how often it has moved. */
int xbox_FramebufferMouse(float *x, float *y)
{
    *x = fbw_mouse_x;
    *y = fbw_mouse_y;
    return (int)fbw_mouse_moves;
}

extern float nv2a_d3d_display_aspect(void);

void fbw_initial_size(int work_w, int work_h, int *w, int *h)
{
    const char *e = getenv("RECOMP_WINDOW_SIZE");
    if (e && sscanf(e, "%dx%d", w, h) == 2 && *w >= 64 && *h >= 48)
        return;
    if (work_h < 600) {
        *h = 720;
        *w = (int)(720 * nv2a_d3d_display_aspect() + 0.5f);
        return;
    }
    *h = work_h * 2 / 3;
    *w = (int)(*h * nv2a_d3d_display_aspect() + 0.5f);
    if (*w > work_w) {
        *w = work_w;
        *h = (int)(*w / nv2a_d3d_display_aspect() + 0.5f);
    }
}
