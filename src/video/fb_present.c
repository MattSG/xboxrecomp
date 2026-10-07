/**
 * The framebuffer window: the title's display, on the host.
 *
 * The title renders through the NV2A and tells the kernel where its display
 * buffer is through AvSetDisplayMode; on hardware the CRTC scans that memory
 * out and the PVIDEO overlay composites movies over it. This file owns the
 * window the result is shown in -- creation, messages, keys, fullscreen -- and
 * keeps track of what the display is showing: flips, buffers the title draws
 * with the CPU instead of flipping, and PVIDEO submissions.
 *
 * The pixels themselves are put on screen by the D3D11 renderer
 * (nv2a_d3d11.c), which asks this file for the scan-out and overlay state when
 * it presents.
 *
 * Off unless RECOMP_FB_WINDOW is set.
 */
#include <stdint.h>
#include "fb_present.h"

#if defined(_WIN32)
#include <windows.h>
#include <stdio.h>
#include "platform/recomp_profile.h"
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include "../nv2a/nv2a_regs.h"

extern ptrdiff_t xbox_GetMemoryOffset(void);
extern bool nv2a_hook_pvideo_snapshot(uint32_t regs[0x1000 / 4]);
extern void nv2a_hook_pvideo_consume(unsigned bank);

static volatile LONG s_fb_running;
static volatile HWND s_hwnd;
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

HWND xbox_FramebufferWindowHandle(void)
{
    return s_hwnd;
}

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
        if (!bytes || physical + bytes > 0x04000000u) continue;
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
                        fwrite(snapshot, 1, (size_t)bytes, f);
                        fclose(f);
                    }
                    fprintf(stderr,
                            "  [PVIDEO] captured frame=%ld bank=%u physical=0x%llX format=0x%08X pitch=%u size=%ux%u bytes=%llu\n",
                            frame, bank, (unsigned long long)physical,
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

/* Which keys are down, for the pad stand-in in src/input.
 *
 * GetAsyncKeyState looked like the cheaper way to ask and does not work
 * here: it reads a state Wine keeps for the X server, and a guest process
 * drawing to a window never sees it change. The window that has the focus
 * is the thing that receives the keys, so that is what has to remember them.
 *
 * Reading this needs no lock. Each entry is written only by the window
 * thread and read only by the USB thread, one byte at a time, and a press
 * seen a frame late is indistinguishable from one made a frame later. */
static volatile unsigned char s_key_down[256];

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

int xbox_FramebufferKeyDown(int vk)
{
    if ((unsigned)vk > 255)
        return 0;
    return s_key_down[vk] != 0;
}

/* Borderless fullscreen on the monitor the window is on, and back. A
 * borderless window rather than DXGI exclusive mode: no mode switch, no
 * device loss, and alt-tab behaves. The renderer resizes its swap chain to
 * whatever the client area becomes. */
static void fb_toggle_fullscreen(HWND h)
{
    static WINDOWPLACEMENT saved = { sizeof(WINDOWPLACEMENT) };
    LONG style = GetWindowLongA(h, GWL_STYLE);
    if (style & WS_OVERLAPPEDWINDOW) {
        MONITORINFO mi = { sizeof mi };
        if (GetWindowPlacement(h, &saved) &&
            GetMonitorInfoA(MonitorFromWindow(h, MONITOR_DEFAULTTOPRIMARY), &mi)) {
            SetWindowLongA(h, GWL_STYLE, style & ~WS_OVERLAPPEDWINDOW);
            SetWindowPos(h, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                         mi.rcMonitor.right - mi.rcMonitor.left,
                         mi.rcMonitor.bottom - mi.rcMonitor.top,
                         SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        }
    } else {
        SetWindowLongA(h, GWL_STYLE, style | WS_OVERLAPPEDWINDOW);
        SetWindowPlacement(h, &saved);
        SetWindowPos(h, NULL, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                     SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    }
}

static LRESULT CALLBACK fb_wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_CLOSE:
    case WM_DESTROY:
        InterlockedExchange(&s_fb_running, 0);
        return 0;

    case WM_ERASEBKGND:
        return 1;                       /* the swap chain owns every pixel */

    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        if ((w == VK_RETURN && (l & (1 << 29))) || w == VK_F11) {
            if (!(l & (1 << 30)))       /* not auto-repeat */
                fb_toggle_fullscreen(h);
            return 0;
        }
        if ((unsigned)w < 256)
            s_key_down[w] = 1;
        /* RECOMP_KEY_TRACE: each key as it arrives, edge-triggered. Sampling
         * which keys are held cannot tell a key never pressed from one that
         * was tapped between samples. */
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
        if ((unsigned)w < 256)
            s_key_down[w] = 0;
        return m == WM_SYSKEYUP ? DefWindowProcA(h, m, w, l) : 0;

    /* Alt-tabbing away with a key held would leave it held for ever. */
    case WM_KILLFOCUS:
        memset((void *)s_key_down, 0, sizeof s_key_down);
        return 0;
    }
    return DefWindowProcA(h, m, w, l);
}

/* The initial client size: RECOMP_WINDOW_SIZE=WxH, else the display aspect
 * at about two thirds of the work area's height. */
extern float nv2a_d3d_display_aspect(void);

static void fb_initial_size(int *w, int *h)
{
    const char *e = getenv("RECOMP_WINDOW_SIZE");
    RECT work;
    if (e && sscanf(e, "%dx%d", w, h) == 2 && *w >= 64 && *h >= 48)
        return;
    if (!SystemParametersInfoA(SPI_GETWORKAREA, 0, &work, 0) ||
        work.bottom - work.top < 600) {
        *h = 720;
        *w = (int)(720 * nv2a_d3d_display_aspect() + 0.5f);
        return;
    }
    *h = (work.bottom - work.top) * 2 / 3;
    *w = (int)(*h * nv2a_d3d_display_aspect() + 0.5f);
    if (*w > work.right - work.left) {
        *w = work.right - work.left;
        *h = (int)(*w / nv2a_d3d_display_aspect() + 0.5f);
    }
}

static DWORD WINAPI fb_thread(LPVOID unused)
{
    HWND hwnd;
    RECT r;
    int cw, ch;

    (void)unused;
    /* Real pixels: without this a scaled desktop hands the swap chain a
     * smaller client area and stretches it back up blurred. */
    SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    {
        WNDCLASSA wc;
        memset(&wc, 0, sizeof(wc));
        wc.lpfnWndProc   = fb_wndproc;
        wc.hInstance     = GetModuleHandleA(NULL);
        wc.hCursor       = LoadCursorA(NULL, IDC_ARROW);
        wc.lpszClassName = "XboxRecompFramebuffer";
        RegisterClassA(&wc);
    }
    fb_initial_size(&cw, &ch);
    r.left = 0; r.top = 0; r.right = cw; r.bottom = ch;
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    hwnd = CreateWindowExA(0, "XboxRecompFramebuffer", "Xbox Recomp - Framebuffer",
                           WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                           CW_USEDEFAULT, CW_USEDEFAULT,
                           r.right - r.left, r.bottom - r.top,
                           NULL, NULL, GetModuleHandleA(NULL), NULL);
    if (!hwnd) {
        InterlockedExchange(&s_fb_running, 0);
        return 0;
    }
    if (getenv("RECOMP_FULLSCREEN"))
        fb_toggle_fullscreen(hwnd);
    s_hwnd = hwnd;
    fprintf(stderr, "  [FBWIN] framebuffer window open (%dx%d client)\n", cw, ch);

    while (InterlockedCompareExchange(&s_fb_running, 1, 1)) {
        MSG msg;
        DWORD wait = MsgWaitForMultipleObjects(0, NULL, FALSE, 250, QS_ALLINPUT);
        (void)wait;
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
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
    }

    s_hwnd = NULL;
    DestroyWindow(hwnd);
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
void xbox_FramebufferNoteFlip(uint32_t draws) { (void)draws; }
int xbox_FramebufferScanSource(uint32_t *va, uint32_t *pitch) { (void)va; (void)pitch; return 0; }
int xbox_FramebufferOverlay(XboxOverlay *o) { (void)o; return 0; }
#endif
