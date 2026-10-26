/**
 * The framebuffer window on Windows, for the D3D11 renderer: creation,
 * messages, keys, fullscreen. The display state it feeds is fb_present.c.
 *
 * Off unless RECOMP_FB_WINDOW is set.
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fb_window.h"

static volatile LONG s_fb_running;
static volatile HWND s_hwnd;

HWND xbox_FramebufferWindowHandle(void)
{
    return s_hwnd;
}

void *fbw_native_window(void)
{
    return (void *)s_hwnd;
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
    int consumed = fbw_host_overlay.message && fbw_host_overlay.message(h, m, w, l);
    if (consumed && m != WM_KILLFOCUS && m != WM_ACTIVATEAPP &&
        m != WM_CLOSE && m != WM_DESTROY) {
        xbox_InputKeysClear();
        return m == WM_SYSKEYDOWN || m == WM_SYSKEYUP ? DefWindowProcA(h, m, w, l) : 0;
    }
    switch (m) {
    case WM_SETCURSOR:
        if (LOWORD(l) == HTCLIENT && fbw_cursor_handler) {
            SetCursor(fbw_cursor_handler());
            return TRUE;
        }
        break;
    case WM_CLOSE:
        if (fbw_close_handler)
            fbw_close_handler();
        InterlockedExchange(&s_fb_running, 0);
        return 0;

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
        xbox_InputKeySet((int)w, 1);
        /* A Shift, Ctrl or Alt is also its side's key, for bindings to
         * "lshift" and the like; wParam stays the either-side one. */
        {
            int side = xbox_InputWin32SideVk((int)w, (uint32_t)l);
            if (side)
                xbox_InputKeySet(side, 1);
        }
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
        xbox_InputKeySet((int)w, 0);
        {
            int side = xbox_InputWin32SideVk((int)w, (uint32_t)l);
            if (side)
                xbox_InputKeySet(side, 0);
            /* With both Shifts down Windows sends one key-up, for the
             * last released, so the other side would stay held. */
            if (w == VK_SHIFT) {
                xbox_InputKeySet(VK_LSHIFT, 0);
                xbox_InputKeySet(VK_RSHIFT, 0);
            }
        }
        return m == WM_SYSKEYUP ? DefWindowProcA(h, m, w, l) : 0;

    /* Where the pointer is, for menu hover (xinput_device.c). */
    case WM_MOUSEMOVE: {
        /* Windows also sends this when the window appears or activates
         * under a resting pointer; only a real change of place is a move. */
        static LPARAM last = -1;
        RECT rc;
        if (GetClientRect(h, &rc) && rc.right > 0 && rc.bottom > 0) {
            fbw_mouse_x = (float)(short)LOWORD(l) / (float)rc.right;
            fbw_mouse_y = (float)(short)HIWORD(l) / (float)rc.bottom;
        }
        if (last != -1 && l != last)
            InterlockedIncrement(&fbw_mouse_moves);
        last = l;
        return 0;
    }

    /* Mouse buttons are virtual keys too, so RECOMP_KEYMAP can bind them. */
    case WM_LBUTTONDOWN: case WM_LBUTTONUP:
        {
            RECT rc;
            if (GetClientRect(h, &rc) && rc.right > 0 && rc.bottom > 0) {
                fbw_mouse_x = (float)(short)LOWORD(l) / (float)rc.right;
                fbw_mouse_y = (float)(short)HIWORD(l) / (float)rc.bottom;
            }
        }
        if (m == WM_LBUTTONDOWN && fbw_mouse_press_handler &&
            fbw_mouse_press_handler(fbw_mouse_x, fbw_mouse_y)) return 0;
        xbox_InputKeySet(VK_LBUTTON, m == WM_LBUTTONDOWN);
        return 0;
    case WM_RBUTTONDOWN: case WM_RBUTTONUP:
        xbox_InputKeySet(VK_RBUTTON, m == WM_RBUTTONDOWN);
        return 0;
    case WM_MBUTTONDOWN: case WM_MBUTTONUP:
        xbox_InputKeySet(VK_MBUTTON, m == WM_MBUTTONDOWN);
        return 0;
    case WM_XBUTTONDOWN: case WM_XBUTTONUP:
        xbox_InputKeySet(GET_XBUTTON_WPARAM(w) == XBUTTON1 ? VK_XBUTTON1 : VK_XBUTTON2, m == WM_XBUTTONDOWN);
        return TRUE;

    /* Alt-tabbing away with a key held would leave it held for ever. */
    case WM_KILLFOCUS:
        xbox_InputKeysClear();
        return 0;
    case WM_ACTIVATEAPP:
        InterlockedExchange(&fbw_inactive, w ? 0 : 1);
        break;
    }
    return DefWindowProcA(h, m, w, l);
}

static DWORD WINAPI fb_thread(LPVOID unused)
{
    HWND hwnd;
    RECT r, work;
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
        wc.hIcon         = LoadIconA(wc.hInstance, MAKEINTRESOURCEA(1)); /* the title's, if it has one */
        wc.lpszClassName = "XboxRecompFramebuffer";
        RegisterClassA(&wc);
    }
    if (!SystemParametersInfoA(SPI_GETWORKAREA, 0, &work, 0))
        memset(&work, 0, sizeof work);
    fbw_initial_size(work.right - work.left, work.bottom - work.top, &cw, &ch);
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
        uint16_t title[128];
        DWORD wait = MsgWaitForMultipleObjects(0, NULL, FALSE, 250, QS_ALLINPUT);
        (void)wait;
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        /* Refresh a title cursor when UI changes under a stationary pointer. */
        if (fbw_cursor_handler) {
            POINT point;
            RECT client;
            if (GetCursorPos(&point) && WindowFromPoint(point) == hwnd &&
                GetClientRect(hwnd, &client) && ScreenToClient(hwnd, &point) && PtInRect(&client, point))
                SetCursor(fbw_cursor_handler());
        }
        if (fbw_title_due(title, 128))
            SetWindowTextW(hwnd, (const wchar_t *)title);
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
