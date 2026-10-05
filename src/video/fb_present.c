/**
 * Show the guest framebuffer in a window.
 *
 * The title renders into its own framebuffer in guest RAM and tells the kernel
 * where it is through AvSetDisplayMode; on hardware the CRTC scans that memory
 * out. Nothing here scans anything out, so however much of the GPU is
 * implemented, none of it is observable. This is the other half: a window that
 * reads that memory and puts it on screen.
 *
 * Deliberately plain GDI rather than the D3D8 layer. The point is to display
 * whatever the guest actually wrote, so the fewer stages between guest memory
 * and the screen the better -- and it must keep working while the D3D8 layer
 * is busy with something else, such as the FMV player's own window.
 * RECOMP_D3D_PRESENT=1 shows the same frames through a D3D11 swap chain of
 * the window's own instead (gpu_present.c): scaled on the GPU, vsync-paced.
 *
 * The window opens at RECOMP_WINDOW_SCALE times the frame (default 2, made
 * smaller until it fits the desktop) and can be resized; the frame is
 * letterboxed to its 4:3 shape. F11 or Alt+Enter toggles borderless
 * fullscreen, RECOMP_FULLSCREEN=1 starts in it. Closing the window ends the
 * process, as the SDL window does.
 *
 * Off unless RECOMP_FB_WINDOW is set.
 */
#include <stdint.h>

#if defined(_WIN32)
#include <windows.h>
#include <stdio.h>
#include "platform/recomp_profile.h"
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include "pvideo_scanout.h"

extern ptrdiff_t xbox_GetMemoryOffset(void);
extern bool nv2a_hook_pvideo_snapshot(uint32_t regs[0x1000 / 4]);
extern void nv2a_hook_pvideo_consume(unsigned bank);

static volatile LONG s_fb_running;
static volatile LONG s_pvideo_capture_count;
static DWORD s_pvideo_next_capture_tick;
static uint32_t      s_fb_va, s_fb_pitch, s_fb_width = 640, s_fb_height = 480;
static uint32_t     *s_rgb;           /* converted 32-bit copy for GDI */
/* A consumed submission remains on screen until the next submission or STOP.
 * Keep its pixels, since consuming the bank lets the guest reuse its memory. */
static uint8_t      *s_pvideo_source;
static size_t        s_pvideo_bytes;
static unsigned      s_pvideo_bank;
static uint32_t      s_pvideo_regs[0x1000 / 4];

/* A finished frame, taken at the flip and shown until the next one.
 *
 * The window used to convert straight out of guest memory every 16 ms. Even
 * pointed at the buffer the title had just finished, that races the executor
 * drawing the next frame into the other one and, whenever the two swap, puts
 * a half-drawn image on the screen -- which is the flicker. Copying the
 * finished frame once per flip means the window never reads memory the
 * rasteriser is writing, so what it shows cannot be half of anything.
 *
 * Two buffers and an index, swapped after the copy completes, so the window
 * thread is never reading the one being filled. */
static uint32_t     *s_present[2];
static volatile LONG s_present_idx = -1;   /* -1 until the first flip */

/* A buffer the title pointed the display at itself, read live until it flips
 * again.
 *
 * A flip is not the only way a picture changes. Between flips a title can
 * set the display mode on a buffer and draw into it with the CPU -- MM3's
 * loading screen does, streaming Startup.tga into the display D3D persisted
 * at the end of the intro. The copy from the last flip showed that a quarter
 * loaded for the whole load. Nothing rasterises into a buffer the title is
 * showing this way, so reading it live is what the CRTC would scan. */
static volatile LONG s_scan_va;            /* 0 while flips are in charge */
static uint32_t      s_scan_pitch;

/* Optional per-submission thumbnails for detecting corruption between screenshots.
 * Each record is a 160x120 BGRA image of the pixels sent to GDI. Keeping this
 * disabled has no file IO; the capture never modifies guest memory. */
static void pvideo_capture_submission(void)
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
    if (!capture) return;
    for (y = 0; y < 120; ++y) {
        unsigned sy = (unsigned)((uint64_t)y * s_fb_height / 120);
        for (x = 0; x < 160; ++x) {
            unsigned sx = (unsigned)((uint64_t)x * s_fb_width / 160);
            row[x] = s_rgb[(size_t)sy * s_fb_width + sx];
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

static void fb_overlay(void)
{
    uint32_t regs[0x1000 / 4];
    unsigned bank;
    bool presented = false;
    if (!s_rgb || !nv2a_hook_pvideo_snapshot(regs)) return;
    {
        static int trace = -1;
        static uint32_t previous[9];
        uint32_t current[9] = {
            regs[NV_PVIDEO_STOP / 4],
            regs[NV_PVIDEO_BASE / 4], regs[(NV_PVIDEO_BASE + 4) / 4],
            regs[NV_PVIDEO_OFFSET / 4], regs[(NV_PVIDEO_OFFSET + 4) / 4],
            regs[NV_PVIDEO_SIZE_IN / 4], regs[(NV_PVIDEO_SIZE_IN + 4) / 4],
            s_fb_va, s_fb_pitch
        };
        if (trace < 0) trace = getenv("RECOMP_PVIDEO_TRACE") != NULL;
        if (trace && memcmp(previous, current, sizeof current)) {
            fprintf(stderr, "[PVIDEO_SCANOUT] fb=%08X pitch=%u present=%ld "
                    "pending=%08X stop=%08X size=%08X/%08X "
                    "offset=%08X/%08X retained=%u pixel=%08X\n",
                    s_fb_va, s_fb_pitch, s_present_idx,
                    regs[NV_PVIDEO_BUFFER / 4], current[0], current[5], current[6],
                    current[3], current[4], s_pvideo_source != NULL,
                    s_rgb[(size_t)s_fb_width * s_fb_height / 2]);
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
        const uint8_t *source;
        const uint8_t *scan_source;
        uint8_t *snapshot = NULL;
        size_t available;
        if (!(regs[NV_PVIDEO_BUFFER / 4] & (1u << (bank * 4u)))) continue;
        if (!bytes || physical + bytes > 0x04000000u) continue;
        va = (uintptr_t)(0x80000000u + (uint32_t)physical) +
             (uintptr_t)xbox_GetMemoryOffset();
        if (!VirtualQuery((const void *)va, &mapping, sizeof(mapping)) ||
            mapping.State != MEM_COMMIT || (mapping.Protect & (PAGE_GUARD | PAGE_NOACCESS)))
            continue;
        available = mapping.RegionSize - (size_t)(va - (uintptr_t)mapping.BaseAddress);
        if (bytes > available) continue;
        source = (const uint8_t *)va;
        scan_source = source;
        if (bytes <= SIZE_MAX) {
            snapshot = (uint8_t *)malloc((size_t)bytes);
            if (snapshot) {
                memcpy(snapshot, source, (size_t)bytes);
                scan_source = snapshot;
            }
        }
        if (!snapshot) continue;
        if (pvideo_scanout(regs, scan_source, available, s_rgb,
                           s_fb_width, s_fb_height, bank)) {
            const char *capture = getenv("RECOMP_PVIDEO_DUMP");
            DWORD now = GetTickCount();
            if (capture && (LONG)(now - s_pvideo_next_capture_tick) >= 0) {
                LONG frame = InterlockedIncrement(&s_pvideo_capture_count);
                s_pvideo_next_capture_tick = now + 500;
                if (frame <= 12) {
                    char path[1024];
                    FILE *f;
                    int n;
                    size_t i, changed = 0;
                    if (snapshot) {
                        for (i = 0; i < (size_t)bytes; ++i)
                            changed += snapshot[i] != source[i];
                    }
                    n = snprintf(path, sizeof(path), "%s.frame%02ld.bank%u.regs.bin",
                                 capture, frame, bank);
                    f = n > 0 && (size_t)n < sizeof(path) ? fopen(path, "wb") : NULL;
                    if (f) {
                        fwrite(regs, 1, sizeof(regs), f);
                        fclose(f);
                    }
                    n = snprintf(path, sizeof(path), "%s.frame%02ld.bank%u.yuy2",
                                 capture, frame, bank);
                    f = n > 0 && (size_t)n < sizeof(path) ? fopen(path, "wb") : NULL;
                    if (f) {
                        fwrite(scan_source, 1, (size_t)bytes, f);
                        fclose(f);
                    }
                    fprintf(stderr,
                            "  [PVIDEO] captured frame=%ld bank=%u physical=0x%llX format=0x%08X pitch=%u size=%ux%u source=%p bytes=%llu changed=%llu\n",
                            frame, bank, (unsigned long long)physical,
                            regs[(NV_PVIDEO_FORMAT + bank * 4) / 4], pitch,
                            regs[(NV_PVIDEO_SIZE_IN + bank * 4) / 4] & 0x7ff,
                            height, (const void *)source,
                            (unsigned long long)bytes,
                            (unsigned long long)changed);
                }
            }
            free(s_pvideo_source);
            s_pvideo_source = snapshot;
            snapshot = NULL;
            s_pvideo_bytes = (size_t)bytes;
            s_pvideo_bank = bank;
            memcpy(s_pvideo_regs, regs, sizeof(s_pvideo_regs));
            presented = true;
            pvideo_capture_submission();
            nv2a_hook_pvideo_consume(bank);
        }
        free(snapshot);
    }
    if (!presented && s_pvideo_source)
        pvideo_scanout(s_pvideo_regs, s_pvideo_source, s_pvideo_bytes, s_rgb,
                       s_fb_width, s_fb_height, s_pvideo_bank);
}

void xbox_FramebufferWindowSet(uint32_t fb_va, uint32_t pitch)
{
    /* RECOMP_FB_VA pins the window to one guest address instead of following
     * whichever surface is being drawn into. A black window cannot distinguish
     * "the read path is broken" from "the title rendered black", and pointing
     * it at memory known to have content settles that. */
    const char *pin = getenv("RECOMP_FB_VA");

    s_fb_va = pin ? (uint32_t)strtoul(pin, NULL, 0) : fb_va;
    if (pitch)
        s_fb_pitch = pitch;
}

static int fb_dump_pixels(const char *path, const uint32_t *pixels);

/* Optional bounded capture of the exact completed image, before publication.
 * RECOMP_PRESENT_CAPTURE=<prefix>; create <prefix>.flag to capture 60 flips. */
static void fb_capture_present(const uint32_t *pixels)
{
    static const char *prefix;
    static int initialized;
    static unsigned remaining, frame;
    char path[1024];
    FILE *flag;
    if (!initialized) {
        prefix = getenv("RECOMP_PRESENT_CAPTURE");
        initialized = 1;
    }
    if (!prefix) return;
    if (!remaining) {
        snprintf(path, sizeof path, "%s.flag", prefix);
        flag = fopen(path, "rb");
        if (!flag) return;
        fclose(flag);
        remove(path);
        remaining = 60;
        frame = 0;
    }
    snprintf(path, sizeof path, "%s-%03u.bmp", prefix, frame++);
    fb_dump_pixels(path, pixels);
    --remaining;
}

/* Called by the pushbuffer executor when the title flips. */
void xbox_FramebufferWindowPresent(uint32_t fb_va, uint32_t pitch)
{
    const uint8_t *src;
    LONG next;
    uint32_t bpp, x, y;

    if (!s_fb_running || !fb_va || !pitch)
        return;
    if (getenv("RECOMP_FB_VA"))
        return;                       /* pinned: leave the old path alone */
    RECOMP_PROFILE_BEGIN("Present copy");
    next = (s_present_idx == 0) ? 1 : 0;
    if (!s_present[next]) {
        s_present[next] = (uint32_t *)calloc((size_t)s_fb_width * s_fb_height,
                                             4);
        if (!s_present[next]) {
            RECOMP_PROFILE_END();
            return;
        }
    }
    bpp = pitch / s_fb_width;
    src = (const uint8_t *)((uintptr_t)fb_va + xbox_GetMemoryOffset());
    for (y = 0; y < s_fb_height; y++) {
        const uint8_t *row = src + (size_t)y * pitch * (bpp == 8 ? s_fb_aa_sy : 1);
        uint32_t *dst = s_present[next] + (size_t)y * s_fb_width;

        if (bpp == 4) {
            memcpy(dst, row, (size_t)s_fb_width * 4);
        } else if (bpp == 8) {
            /* 2x-wide anti-aliased 32-bit surface: every other pixel, as in
             * fb_convert. */
            const uint32_t *p = (const uint32_t *)row;
            for (x = 0; x < s_fb_width; x++)
                dst[x] = p[x * 2];
        } else if (bpp == 2) {
            const uint16_t *p = (const uint16_t *)row;
            for (x = 0; x < s_fb_width; x++) {
                uint16_t v = p[x];
                uint32_t r = (uint32_t)((v >> 11) & 0x1F) * 255u / 31u;
                uint32_t g = (uint32_t)((v >>  5) & 0x3F) * 255u / 63u;
                uint32_t b = (uint32_t)( v        & 0x1F) * 255u / 31u;
                dst[x] = (r << 16) | (g << 8) | b;
            }
        } else {
            memset(dst, 0, (size_t)s_fb_width * 4);
        }
    }
    fb_capture_present(s_present[next]);
    /* Published only once it is whole. */
    InterlockedExchange(&s_present_idx, next);
    /* Presenting the buffer already on display hands nothing over: D3D
     * persisting its display flips the very buffer it just set the mode on,
     * and the title then keeps drawing into it with no further flips. */
    if ((uint32_t)s_scan_va != fb_va)
    InterlockedExchange(&s_scan_va, 0);
    RECOMP_PROFILE_END();
}

/* Called from AvSetDisplayMode. Before the first flip the window already
 * reads guest memory live, following the draw target, so this only matters
 * once flips have taken over. */
void xbox_FramebufferWindowScanout(uint32_t fb_va, uint32_t pitch)
{
    if (s_present_idx < 0 || !fb_va || !pitch)
        return;
    s_scan_pitch = pitch;
    InterlockedExchange(&s_scan_va, (LONG)fb_va);
}

/* Title bar, the way ps3recomp's window shows it. Written by the flip,
 * read once a second by the window thread; a torn read shows one stale
 * number for a second, which nobody can tell apart from a real one. */
static wchar_t       s_title[48] = L"Xbox Recomp";
static volatile LONG s_flips;
static volatile LONG s_frame_draws;

void xbox_FramebufferWindowSetTitle(const uint16_t *name, int max_chars)
{
    int i;
    for (i = 0; i < max_chars && i < 47 && name[i]; i++)
        s_title[i] = (wchar_t)name[i];
    if (i)
        s_title[i] = 0;
}

void xbox_FramebufferWindowFrameStats(uint32_t draws)
{
    static LARGE_INTEGER frequency, previous;
    static FILE *frame_log;
    static unsigned samples;
    LARGE_INTEGER now;
    if (!frequency.QuadPart) {
        const char *path = getenv("RECOMP_FRAME_TIMES");
        QueryPerformanceFrequency(&frequency);
        if (path) {
            frame_log = fopen(path, "w");
            if (frame_log) fprintf(frame_log, "qpc_ms,frame_ms,draws\n");
        }
    }
    QueryPerformanceCounter(&now);
    if (previous.QuadPart) {
        double ms = (now.QuadPart - previous.QuadPart) * 1000.0 / frequency.QuadPart;
        RECOMP_PROFILE_PLOT("Frame interval (ms)", ms);
        if (frame_log) {
            fprintf(frame_log, "%.6f,%.6f,%u\n", now.QuadPart * 1000.0 / frequency.QuadPart, ms, draws);
            if (++samples % 30 == 0) fflush(frame_log);
        }
    }
    previous = now;
    RECOMP_PROFILE_FRAME();
    InterlockedIncrement(&s_flips);
    InterlockedExchange(&s_frame_draws, (LONG)draws);
}

/* Alt+Enter / F11: borderless fullscreen on the window's monitor, and back.
 * (DXGI's own Alt+Enter, exclusive mode at the frame's 640x480, is off --
 * gpu_present.c; both presenters letterbox to any size.) */
static void fb_toggle_fullscreen(HWND h)
{
    static int full;
    static RECT saved;
    static LONG_PTR saved_style;
    if (!full) {
        MONITORINFO mi;
        mi.cbSize = sizeof mi;
        if (!GetMonitorInfoA(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi))
            return;
        GetWindowRect(h, &saved);
        saved_style = GetWindowLongPtrA(h, GWL_STYLE);
        SetWindowLongPtrA(h, GWL_STYLE, (saved_style & ~WS_OVERLAPPEDWINDOW) | WS_POPUP);
        SetWindowPos(h, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                     mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top,
                     SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        full = 1;
    } else {
        SetWindowLongPtrA(h, GWL_STYLE, saved_style);
        SetWindowPos(h, NULL, saved.left, saved.top, saved.right - saved.left,
                     saved.bottom - saved.top, SWP_FRAMECHANGED | SWP_NOZORDER | SWP_SHOWWINDOW);
        full = 0;
    }
}

static LRESULT CALLBACK fb_wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    /* First press only (bit 30: the key was already down), and with Alt held
     * (bit 29) for Enter; neither reaches the key table. */
    if ((m == WM_SYSKEYDOWN && w == VK_RETURN && (l & (1 << 29)) && !(l & (1 << 30)))
        || (m == WM_KEYDOWN && w == VK_F11 && !(l & (1 << 30)))) {
        fb_toggle_fullscreen(h);
        return 0;
    }
    if (m == WM_SYSKEYDOWN && w == VK_RETURN)
        return 0;                       /* auto-repeat of Alt+Enter */
    switch (m) {
    case WM_CLOSE:
    case WM_DESTROY:
        /* Closing the window ends the run. Only stopping this thread left
         * every other one running -- audio played on, and the process had
         * to be ended from Task Manager. The guest has no exit path of its
         * own to ask. TerminateProcess rather than ExitProcess: the guest
         * threads are still running, and ExitProcess's cleanup can wait on
         * a lock one of them held. The exit hook does the reporting that
         * would otherwise be skipped. RECOMP_CLOSE_KEEPS_RUNNING=1 only
         * closes the window, as before. */
        InterlockedExchange(&s_fb_running, 0);
        if (!getenv("RECOMP_CLOSE_KEEPS_RUNNING")) {
            fprintf(stderr, "  [FBWIN] window closed: exiting\n");
            recomp_exit_hook_run("window");
            fflush(stdout);
            fflush(stderr);
            TerminateProcess(GetCurrentProcess(), 0);
        }
        return 0;

    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        xbox_InputKeySet((int)w, 1);
        /* A Shift, Ctrl or Alt is also its side's key, for bindings to
         * "lshift" and the like; wParam stays the either-side one. */
        {
            int side = xbox_InputWin32SideVk((int)w, (uint32_t)l);
            if (side)
                xbox_InputKeySet(side, 1);
        }
        /* RECOMP_KEY_TRACE: each key as it arrives, edge-triggered.
         *
         * The obvious diagnostic -- sampling which keys are held, once a
         * second, from the input path -- cannot tell a key that was never
         * pressed from one that was tapped: a 100 ms press is caught about
         * one time in ten. That ambiguity is expensive when the only way
         * to test is to ask someone to press a key and describe what
         * happened. This answers "did it arrive" on its own. */
        if (getenv("RECOMP_KEY_TRACE")) {
            static unsigned n;
            if (n++ < 40) {
                fprintf(stderr, "  [KEY] down vk=0x%02X\n", (unsigned)w);
                fflush(stderr);
            }
        }
        /* System keys still go to Windows, or Alt+F4 stops closing us. */
        return m == WM_SYSKEYDOWN ? DefWindowProcA(h, m, w, l) : 0;

    case WM_KEYUP:
    case WM_SYSKEYUP:
        xbox_InputKeySet((int)w, 0);
        {
            int side = xbox_InputWin32SideVk((int)w, (uint32_t)l);
            if (side)
                xbox_InputKeySet(side, 0);
            /* With both Shifts down Windows sends one key-up, for the
             * last released, so the other side would stay held. Autorepeat
             * sets a Shift still down again. */
            if (w == VK_SHIFT) {
                xbox_InputKeySet(VK_LSHIFT, 0);
                xbox_InputKeySet(VK_RSHIFT, 0);
            }
        }
        return m == WM_SYSKEYUP ? DefWindowProcA(h, m, w, l) : 0;

    /* Mouse buttons go in the same table, for bindings that name them.
     * Capturing the mouse while one is down delivers its release even
     * outside the window. */
    case WM_LBUTTONDOWN: case WM_LBUTTONUP:
    case WM_RBUTTONDOWN: case WM_RBUTTONUP:
    case WM_MBUTTONDOWN: case WM_MBUTTONUP:
    case WM_XBUTTONDOWN: case WM_XBUTTONUP:
        {
            int down = 0;
            int vk = xbox_InputWin32MouseVk(m, (uint32_t)w, &down);
            if (vk)
                xbox_InputKeySet(vk, down);
            if (down)
                SetCapture(h);
            else if (!(w & (MK_LBUTTON | MK_RBUTTON | MK_MBUTTON | MK_XBUTTON1 | MK_XBUTTON2)))
                ReleaseCapture();
        }
        return m == WM_XBUTTONDOWN || m == WM_XBUTTONUP ? TRUE : 0;

    /* Alt-tabbing away with a key held would leave it held for ever. */
    case WM_KILLFOCUS:
        xbox_InputKeysClear();
        return 0;
    }
    return DefWindowProcA(h, m, w, l);
}

/* The display formats an Xbox front buffer is actually set to. The pitch says
 * how wide a row is in bytes, so pitch/width gives the pixel size; the exact
 * component layout only matters for 16-bit, where 5:6:5 and 1:5:5:5 differ. */
static void fb_convert(const uint8_t *src, uint32_t pitch)
{
    uint32_t bpp = pitch / s_fb_width;
    uint32_t x, y;

    for (y = 0; y < s_fb_height; y++) {
        const uint8_t *row = src + (size_t)y * pitch;
        uint32_t *dst = s_rgb + (size_t)y * s_fb_width;

        if (bpp == 4) {
            memcpy(dst, row, (size_t)s_fb_width * 4);
        } else if (bpp == 8) {
            /* A 2x-wide anti-aliased 32-bit surface (pitch = 2 * width * 4):
             * take every other pixel. X-Men Legends renders its 640-wide
             * frame into a 1280-wide surface, which read as "8 bytes per
             * pixel" and showed black. */
            const uint32_t *p = (const uint32_t *)row;
            for (x = 0; x < s_fb_width; x++)
                dst[x] = p[x * 2];
        } else if (bpp == 2) {
            const uint16_t *p = (const uint16_t *)row;
            for (x = 0; x < s_fb_width; x++) {
                uint16_t v = p[x];
                uint32_t r = (uint32_t)((v >> 11) & 0x1F) * 255u / 31u;
                uint32_t g = (uint32_t)((v >>  5) & 0x3F) * 255u / 63u;
                uint32_t b = (uint32_t)( v        & 0x1F) * 255u / 31u;
                dst[x] = (r << 16) | (g << 8) | b;
            }
        } else {
            memset(dst, 0, (size_t)s_fb_width * 4);
        }
    }
}

/* Write what the window is currently showing to a 24-bit BMP.
 *
 * A black window is ambiguous: it means either that the read path is wrong or
 * that the title really did render black. Dumping the same converted pixels
 * the window draws settles which, and does it without a screenshot. */
static int fb_dump_pixels(const char *path, const uint32_t *pixels)
{
    FILE *f;
    uint32_t row = ((s_fb_width * 3u) + 3u) & ~3u;
    uint32_t img = row * s_fb_height, total = 54u + img, y, x;
    uint8_t hdr[54], *line;

    if (!pixels)
        return -1;
    f = fopen(path, "wb");
    if (!f)
        return -1;
    memset(hdr, 0, sizeof(hdr));
    hdr[0] = 'B'; hdr[1] = 'M';
    memcpy(hdr + 2, &total, 4);
    hdr[10] = 54; hdr[14] = 40;
    memcpy(hdr + 18, &s_fb_width, 4);
    memcpy(hdr + 22, &s_fb_height, 4);
    hdr[26] = 1; hdr[28] = 24;
    memcpy(hdr + 34, &img, 4);
    fwrite(hdr, 1, sizeof(hdr), f);

    line = (uint8_t *)calloc(1, row);
    for (y = 0; y < s_fb_height; y++) {
        const uint32_t *src = pixels + (size_t)(s_fb_height - 1 - y) * s_fb_width;
        for (x = 0; x < s_fb_width; x++) {
            line[x * 3 + 0] = (uint8_t)(src[x] & 0xFF);
            line[x * 3 + 1] = (uint8_t)((src[x] >> 8) & 0xFF);
            line[x * 3 + 2] = (uint8_t)((src[x] >> 16) & 0xFF);
        }
        fwrite(line, 1, row, f);
    }
    free(line);
    fclose(f);
    fprintf(stderr, "  [FBWIN] wrote %s (%ux%u from 0x%08X)\n",
            path, s_fb_width, s_fb_height, s_fb_va);
    return 0;
}

int xbox_FramebufferDumpBmp(const char *path)
{
    return fb_dump_pixels(path, s_rgb);
}

static DWORD WINAPI fb_thread(LPVOID unused)
{
    HWND hwnd;
    HDC hdc;
    BITMAPINFO bi;
    RECT r;
    int fb_pinned;

    (void)unused;

    {
        WNDCLASSA wc;
        memset(&wc, 0, sizeof(wc));
        wc.lpfnWndProc   = fb_wndproc;
        wc.hInstance     = GetModuleHandleA(NULL);
        wc.hCursor       = LoadCursorA(NULL, IDC_ARROW);
        wc.lpszClassName = "XboxRecompFramebuffer";
        RegisterClassA(&wc);
    }
    {
        /* RECOMP_WINDOW_SCALE (1-8, default 2, as in the SDL window): the
         * client area is the frame times this, made smaller until the window
         * fits the desktop work area, and centred on it. */
        const char *e = getenv("RECOMP_WINDOW_SCALE");
        int scale = e ? atoi(e) : 2;
        RECT work;
        if (scale < 1 || scale > 8)
            scale = 2;
        if (!SystemParametersInfoA(SPI_GETWORKAREA, 0, &work, 0)) {
            work.left = work.top = 0; work.right = 1920; work.bottom = 1080;
        }
        for (;; scale--) {
            r.left = 0; r.top = 0;
            r.right = (LONG)s_fb_width * scale; r.bottom = (LONG)s_fb_height * scale;
            AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
            if (scale <= 1 || (r.right - r.left <= work.right - work.left
                               && r.bottom - r.top <= work.bottom - work.top))
                break;
        }
        hwnd = CreateWindowExA(0, "XboxRecompFramebuffer", "Xbox Recomp - Framebuffer",
                               WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                               work.left + ((work.right - work.left) - (r.right - r.left)) / 2,
                               work.top + ((work.bottom - work.top) - (r.bottom - r.top)) / 2,
                               r.right - r.left, r.bottom - r.top,
                               NULL, NULL, GetModuleHandleA(NULL), NULL);
    }
    if (!hwnd) {
        InterlockedExchange(&s_fb_running, 0);
        return 0;
    }
    hdc = GetDC(hwnd);
    {
        const char *fs = getenv("RECOMP_FULLSCREEN");
        if (fs && *fs && *fs != '0')
            fb_toggle_fullscreen(hwnd);
    }

    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize        = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth       = (LONG)s_fb_width;
    bi.bmiHeader.biHeight      = -(LONG)s_fb_height;   /* top-down */
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    s_rgb = (uint32_t *)calloc((size_t)s_fb_width * s_fb_height, 4);
    /* A window pinned to one address shows that memory and nothing else. */
    fb_pinned = getenv("RECOMP_FB_VA") != NULL;

    fprintf(stderr, "  [FBWIN] framebuffer window open (%ux%u)\n",
            s_fb_width, s_fb_height);
    if (gpu_present_enabled())
        gpu_present_init(hwnd, s_fb_width, s_fb_height);

    while (InterlockedCompareExchange(&s_fb_running, 1, 1)) {
        MSG msg;
        int paced = 0;
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        if (s_scan_va && s_rgb) {
            fb_convert((const uint8_t *)((uintptr_t)(uint32_t)s_scan_va +
                                         xbox_GetMemoryOffset()), s_scan_pitch);
            fb_overlay();
            StretchDIBits(hdc, 0, 0, (int)s_fb_width, (int)s_fb_height,
                          0, 0, (int)s_fb_width, (int)s_fb_height,
                          s_rgb, &bi, DIB_RGB_COLORS, SRCCOPY);
        } else if (s_present_idx >= 0 && s_rgb) {
            /* A finished frame, published by the flip. Copied into s_rgb so
             * the dump path and GDI see one consistent image even if the
             * next flip lands mid-blit. */
            LONG idx = s_present_idx;
            if (s_present[idx])
                memcpy(s_rgb, s_present[idx],
                       (size_t)s_fb_width * s_fb_height * 4);
            if (!getenv("RECOMP_FB_VA"))
                fb_overlay();
            StretchDIBits(hdc, 0, 0, (int)s_fb_width, (int)s_fb_height,
                          0, 0, (int)s_fb_width, (int)s_fb_height,
                          s_rgb, &bi, DIB_RGB_COLORS, SRCCOPY);
        } else if (s_fb_va && s_fb_pitch && s_rgb) {
            /* No flip yet, or pinned with RECOMP_FB_VA: read guest memory as
             * before, which is also what a title that never flips needs. */
            const uint8_t *src =
                (const uint8_t *)((uintptr_t)s_fb_va + xbox_GetMemoryOffset());
            fb_convert(src, s_fb_pitch);
            if (!getenv("RECOMP_FB_VA"))
                fb_overlay();
            StretchDIBits(hdc, 0, 0, (int)s_fb_width, (int)s_fb_height,
                          0, 0, (int)s_fb_width, (int)s_fb_height,
                          s_rgb, &bi, DIB_RGB_COLORS, SRCCOPY);
        }
        {
            /* One dump a few seconds in, so the title has had time to render
             * something rather than catching the first blank frame.
             *
             * RECOMP_FB_WINDOW_DUMP_EVERY=<frames> dumps repeatedly instead.
             * This window follows the address AvSetDisplayMode gave, which is
             * what the CRTC scans and therefore what a person sees; the
             * pushbuffer executor's own dump follows its draw surface. With
             * double buffering those are different buffers, and measuring
             * progress from the executor's dump reports a blank screen while
             * the window is showing the title's logo. Ask the window. */
            const char *dump = getenv("RECOMP_FB_DUMP");
            const char *every = getenv("RECOMP_FB_WINDOW_DUMP_EVERY");
            static int frames;
            int period = every ? atoi(every) : 0;
            frames++;
            if (dump && period > 0) {
                if (frames % period == 0)
                    xbox_FramebufferDumpBmp(dump);
            } else if (dump && frames == 600) {
                xbox_FramebufferDumpBmp(dump);
            }
        }
        {
            static DWORD t0;
            static LONG f0;
            DWORD now = GetTickCount();
            if (now - t0 >= 1000) {
                LONG f = s_flips;
                wchar_t tb[128];
                _snwprintf(tb, 127, L"%ls | FPS: %.1f | draws: %ld", s_title,
                           t0 ? (f - f0) * 1000.0 / (now - t0) : 0.0,
                           (long)s_frame_draws);
                tb[127] = 0;
                SetWindowTextW(hwnd, tb);
                t0 = now; f0 = f;
            }
        }
        if (!paced)
            Sleep(16);                  /* D3D11's Present waited for vsync */
    }

    gpu_present_shutdown();
    ReleaseDC(hwnd, hdc);
    DestroyWindow(hwnd);
    free(s_rgb);
    s_rgb = NULL;
    free(s_pvideo_source);
    s_pvideo_source = NULL;
    return 0;
}

void xbox_FramebufferWindowStart(void)
{
    HANDLE th;

    if (!getenv("RECOMP_FB_WINDOW"))
        return;
    if (InterlockedCompareExchange(&s_fb_running, 1, 0) != 0)
        return;
    th = CreateThread(NULL, 0, fb_thread, NULL, 0, NULL);
    if (th)
        CloseHandle(th);
    else
        InterlockedExchange(&s_fb_running, 0);
}

#else
void xbox_FramebufferWindowSet(uint32_t fb_va, uint32_t pitch) { (void)fb_va; (void)pitch; }
void xbox_FramebufferWindowPresent(uint32_t fb_va, uint32_t pitch) { (void)fb_va; (void)pitch; }
void xbox_FramebufferWindowScanout(uint32_t fb_va, uint32_t pitch) { (void)fb_va; (void)pitch; }
void xbox_FramebufferWindowStart(void) {}
int xbox_FramebufferKeyDown(int vk) { (void)vk; return 0; }
void xbox_FramebufferWindowSetTitle(const uint16_t *n, int m) { (void)n; (void)m; }
void xbox_FramebufferWindowFrameStats(uint32_t draws) { (void)draws; }
#endif
