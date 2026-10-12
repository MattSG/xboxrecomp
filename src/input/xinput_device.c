/**
 * Xbox Input Compatibility Layer
 *
 * Translates the Xbox controller API to a host gamepad backend.
 * Handles the structural differences between the Xbox gamepad
 * (analog buttons as bytes, separate trigger channels) and the host
 * (digital face buttons, trigger axes).
 *
 *   _WIN32 -> Windows XInput
 *   POSIX  -> SDL2 GameController
 */

#include "xinput_xbox.h"
#include "keyboard.h"
#include "mkb_input.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

static XBOX_KEYBOARD_DRIVING_PROBE s_keyboard_driving_probe;

void xbox_InputSetKeyboardDrivingProbe(XBOX_KEYBOARD_DRIVING_PROBE probe)
{
    s_keyboard_driving_probe = probe;
}

/* ======================================================================== */
#if defined(_WIN32)
/* ====================  XInput backend  ================================== */
/* ======================================================================== */

#include <xinput.h>
#pragma comment(lib, "xinput.lib")

static BOOL  g_controller_connected[XBOX_MAX_CONTROLLERS] = { FALSE };
static DWORD g_last_packet[XBOX_MAX_CONTROLLERS] = { 0 };
static DWORD g_packet_bias[XBOX_MAX_CONTROLLERS];
static BOOL g_capture_active[XBOX_MAX_CONTROLLERS];

static DWORD finish_state(DWORD port, XBOX_INPUT_STATE *state)
{
    state->dwPacketNumber += g_packet_bias[port];
    if (g_capture_active[port] && state->dwPacketNumber == g_last_packet[port]) {
        ++g_packet_bias[port];
        ++state->dwPacketNumber;
    }
    g_capture_active[port] = FALSE;
    g_last_packet[port] = state->dwPacketNumber;
    return ERROR_SUCCESS;
}

/* ---- keyboard, when there is no pad --------------------------------------
 *
 * A title that waits on PRESS START is unreachable on a machine with no
 * controller plugged in, which is most machines someone brings this up on.
 * The whole point of a recompilation is to be able to look at the thing
 * running, and a build nobody can press a button in cannot be looked at.
 *
 * Off by default, because a keyboard silently acting as player 1 is
 * surprising when a real pad is what you meant to use. RECOMP_KEYBOARD=1
 * turns it on. It only ever answers for port 0, and is merged on top of a
 * pad connected there, so a real controller keeps working.
 *
 * The keys are the ones a Dreamcast or Saturn emulator would pick, which is
 * the closest thing to a convention here:
 *
 *   arrows        d-pad             Enter      START
 *   Z X A S       A B X Y           Backspace  BACK
 *   Q E           white black       1 3        triggers
 *   numpad 8/2/4/6 left thumb       I/K/J/L    right thumb
 *
 * RECOMP_KEYBOARD_LAYOUT=mm3 maps arrows to driving while the title's
 * driving probe is true: up/down are the right/left triggers and left/right
 * steer. Otherwise they are only menu d-pad buttons. The numbered trigger
 * and numpad stick keys still work in either context. While driving, R/F
 * also alias Xbox B/X (MM3's default manual gear up/down), Space aliases A
 * (handbrake), and C aliases Black (change camera view). Escape aliases B in
 * this layout, matching X's menu-back action. Enter remains Start.
 *
 * Keys come from the framebuffer window, which only receives them while it
 * has the focus, so typing in another window does not drive the game.
 */
static BOOL keyboard_enabled(void)
{
    static int on = -1;
    if (on < 0) {
        const char *v = getenv("RECOMP_KEYBOARD");
        on = (v && *v && *v != '0') ? 1 : 0;
    }
    return on ? TRUE : FALSE;
}

static BOOL keyboard_mm3_layout(void)
{
    static int on = -1;
    if (on < 0) {
        const char *v = getenv("RECOMP_KEYBOARD_LAYOUT");
        on = (v && _stricmp(v, "mm3") == 0) ? 1 : 0;
    }
    return on ? TRUE : FALSE;
}

/* The framebuffer window records what is held; see src/video/fb_present.c.
 *
 * GetAsyncKeyState was the first attempt and reads nothing here -- under
 * Wine it answers about a state a GDI-drawing guest never touches. Going
 * through the window is also the better gate: a window only receives keys
 * while it has the focus, so there is no separate focus check to get wrong
 * and no way for typing in another application to drive the game.
 *
 * Declared rather than included so src/input does not depend on the video
 * module's include path. */
extern int xbox_FramebufferKeyDown(int vk);
extern int xbox_FramebufferHostInputCaptured(void);

static BOOL key_down(int vk)
{
    return xbox_FramebufferKeyDown(vk) ? TRUE : FALSE;
}

/* The bindable inputs, in RECOMP_KEYMAP's names, with the default keys
 * above. RECOMP_KEYMAP=NAME:VK,... (VK in hex) rebinds any of them; mouse
 * buttons are virtual keys too (1 left, 2 right, 4 middle, 5/6 side). */
enum { K_A, K_B, K_X, K_Y, K_WHITE, K_BLACK, K_LT, K_RT, K_START, K_BACK,
       K_UP, K_DOWN, K_LEFT, K_RIGHT, K_LS, K_RS,
       K_LX_NEG, K_LX_POS, K_LY_NEG, K_LY_POS, K_RX_NEG, K_RX_POS, K_RY_NEG, K_RY_POS,
       K_COUNT };
static const char *const s_key_names[K_COUNT] = {
    "A", "B", "X", "Y", "WHITE", "BLACK", "LT", "RT", "START", "BACK",
    "UP", "DOWN", "LEFT", "RIGHT", "LS", "RS",
    "LX-", "LX+", "LY-", "LY+", "RX-", "RX+", "RY-", "RY+" };
static int s_keys[K_COUNT] = {
    'Z', 'X', 'A', 'S', 'Q', 'E', '1', '3', VK_RETURN, VK_BACK,
    VK_UP, VK_DOWN, VK_LEFT, VK_RIGHT, VK_SHIFT, VK_CONTROL,
    VK_NUMPAD4, VK_NUMPAD6, VK_NUMPAD2, VK_NUMPAD8, 'J', 'L', 'K', 'I' };

static void load_keymap(void)
{
    static int loaded;
    const char *p = getenv("RECOMP_KEYMAP");
    if (loaded || !p)
        return;
    loaded = 1;
    while (*p) {
        size_t len = strcspn(p, ":");
        int i;
        if (!p[len])
            break;
        for (i = 0; i < K_COUNT; i++)
            if (strlen(s_key_names[i]) == len && !strncmp(p, s_key_names[i], len))
                s_keys[i] = (int)strtoul(p + len + 1, NULL, 16) & 0xFF;
        p += strcspn(p, ",");
        if (*p)
            p++;
    }
}

/* RECOMP_MENU_KEYS=1: while the title reports a menu (xbox_InputSetMenuProbe;
 * with no probe, always) the arrows, W/A/S/D and the left-stick keys move
 * the D-pad, Space or a left click is A and Esc or a right click is B. Those
 * keys leave their own bindings alone meanwhile, so A in a menu is left, not X. */
static int (*s_in_menu)(void);
static BOOL s_menu;

void xbox_InputSetMenuProbe(int (*in_menu)(void))
{
    s_in_menu = in_menu;
}

static const struct { int arrow, letter, stick; WORD bit; } s_menu_dirs[4] = {
    { VK_UP, 'W', K_LY_POS, XBOX_GAMEPAD_DPAD_UP },
    { VK_DOWN, 'S', K_LY_NEG, XBOX_GAMEPAD_DPAD_DOWN },
    { VK_LEFT, 'A', K_LX_NEG, XBOX_GAMEPAD_DPAD_LEFT },
    { VK_RIGHT, 'D', K_LX_POS, XBOX_GAMEPAD_DPAD_RIGHT } };

static BOOL menu_key(int vk)
{
    int i;
    for (i = 0; i < 4; i++)
        if (vk == s_menu_dirs[i].arrow || vk == s_menu_dirs[i].letter || vk == s_keys[s_menu_dirs[i].stick])
            return TRUE;
    return vk == VK_SPACE || vk == VK_LBUTTON || vk == VK_ESCAPE || vk == VK_RBUTTON;
}

static BOOL menu_keys_enabled(void)
{
    static int on = -1;
    if (on < 0) {
        const char *v = getenv("RECOMP_MENU_KEYS");
        on = (v && *v && *v != '0') ? 1 : 0;
    }
    return on ? TRUE : FALSE;
}

/* Pointing at a menu item moves the focus there: D-pad pulses, a row at a
 * time, until the title's pointer function (xbox_InputSetMenuPointer) sees
 * the focus under the pointer. Only after the pointer moves, so it never
 * fights the keys. A left click is held back until the focus has arrived,
 * then sent as A; *click says when. */
extern int xbox_FramebufferMouse(float *x, float *y);
extern void nv2a_d3d_collect_menu_rects(int on);
static int (*s_menu_pointer)(float x, float y, int *offset);

void xbox_InputSetMenuPointer(int (*pointer)(float x, float y, int *offset))
{
    s_menu_pointer = pointer;
}

static WORD menu_hover(BOOL nav_keys, BOOL *click)
{
    static int seen_moves, active, was_down, pulsed_rows;
    static ULONGLONG pulse_until, pulsed_at, click_at, a_until;
    static WORD bit;
    ULONGLONG now = GetTickCount64();
    float x, y;
    int moves = xbox_FramebufferMouse(&x, &y), rows = 0, on = 0, down = key_down(VK_LBUTTON);
    if (moves != seen_moves) { seen_moves = moves; active = 1; }
    if (nav_keys) active = 0;
    if (active) {
        /* Trust a reading only once three polls agree: the highlight
         * animates, and a frame caught mid-way can point anywhere. */
        static int last_on, last_rows, same;
        on = s_menu_pointer ? s_menu_pointer(x, y, &rows) : 0;
        same = on == last_on && rows == last_rows ? same + 1 : 0;
        last_on = on; last_rows = rows;
        if (same < 2 && on > 0) on = -1;
    }
    /* One row per pulse, the next only once the highlight is seen to have
     * moved (titles fade it over a few hundred ms), or after 700 ms. */
    if (on == 1 && rows && (rows != pulsed_rows || now - pulsed_at > 700) && now >= pulse_until + 30) {
        bit = rows > 0 ? XBOX_GAMEPAD_DPAD_DOWN : XBOX_GAMEPAD_DPAD_UP;
        pulse_until = now + 50;     /* a few title polls held, then let go */
        pulsed_at = now;
        pulsed_rows = rows;
    }
    /* A strip that scrolls (on == 2) moves on a click, not on hover: that
     * many presses left or right, and no A. */
    {
        static int sideways;
        if (down && !was_down && on == 2) {
            sideways = rows;
            down = 0;               /* the click is spent on the move */
        }
        if (sideways && now >= pulse_until + 120) {
            bit = sideways > 0 ? XBOX_GAMEPAD_DPAD_RIGHT : XBOX_GAMEPAD_DPAD_LEFT;
            sideways += sideways > 0 ? -1 : 1;
            pulse_until = now + 50;
        }
    }
    if (down && !was_down) click_at = now;
    was_down = key_down(VK_LBUTTON);
    /* Off an item the click goes straight through; on one it waits for the
     * focus, or 800 ms where the highlight cannot be found. */
    if (click_at && (on == 0 || (on > 0 && !rows) || now - click_at > 800)) {
        click_at = 0;
        a_until = now + 80;
    }
    *click = now < a_until;
    return now < pulse_until ? bit : 0;
}

static BOOL bound_down(int k)
{
    return s_keys[k] && key_down(s_keys[k]) && !(s_menu && menu_key(s_keys[k]));
}

/* A thumb axis from two keys, at the full deflection a digital key implies. */
static SHORT axis_from_keys(int negative, int positive)
{
    int v = 0;
    if (bound_down(negative)) v -= 32767;
    if (bound_down(positive)) v += 32767;
    return (SHORT)v;
}

static void keyboard_state(XBOX_INPUT_STATE *pState)
{
    static DWORD packet;
    static const int analog[8] = { K_A, K_B, K_X, K_Y, K_BLACK, K_WHITE, K_LT, K_RT };
    WORD b = 0, nav = 0;
    BOOL hover_click = FALSE;
    int i;
    BOOL mm3 = keyboard_mm3_layout();
    BOOL driving;

    load_keymap();
    memset(pState, 0, sizeof(*pState));
    s_menu = menu_keys_enabled() && (!s_in_menu || s_in_menu());
    driving = mm3 && !s_menu && s_keyboard_driving_probe && s_keyboard_driving_probe();
    if (menu_keys_enabled() && s_menu_pointer)
        nv2a_d3d_collect_menu_rects(s_menu);

    if (!driving) {
        if (bound_down(K_UP))    b |= XBOX_GAMEPAD_DPAD_UP;
        if (bound_down(K_DOWN))  b |= XBOX_GAMEPAD_DPAD_DOWN;
        if (bound_down(K_LEFT))  b |= XBOX_GAMEPAD_DPAD_LEFT;
        if (bound_down(K_RIGHT)) b |= XBOX_GAMEPAD_DPAD_RIGHT;
    }
    if (bound_down(K_START)) b |= XBOX_GAMEPAD_START;
    if (bound_down(K_BACK))  b |= XBOX_GAMEPAD_BACK;
    if (bound_down(K_LS))    b |= XBOX_GAMEPAD_LEFT_THUMB;
    if (bound_down(K_RS))    b |= XBOX_GAMEPAD_RIGHT_THUMB;
    if (s_menu) {
        for (i = 0; i < 4; i++)
            if (key_down(s_menu_dirs[i].arrow) || key_down(s_menu_dirs[i].letter) ||
                (s_keys[s_menu_dirs[i].stick] && key_down(s_keys[s_menu_dirs[i].stick])))
                nav |= s_menu_dirs[i].bit;
        b |= nav | menu_hover(nav != 0, &hover_click);
    }
    pState->Gamepad.wButtons = b;

    /* Analog on the console, so a key is 255 rather than a flag -- a title
     * that reads these as a pressure never sees a press if they are 1.
     * Indexed by XBOX_BUTTON_A..RTRIGGER. */
    for (i = 0; i < 8; i++)
        pState->Gamepad.bAnalogButtons[i] = bound_down(analog[i]) ? 255 : 0;
    if (s_menu && (key_down(VK_SPACE) || hover_click))
        pState->Gamepad.bAnalogButtons[XBOX_BUTTON_A] = 255;
    if (s_menu && (key_down(VK_ESCAPE) || key_down(VK_RBUTTON)))
        pState->Gamepad.bAnalogButtons[XBOX_BUTTON_B] = 255;

    /* Retain MM3's contextual driving aliases alongside the user's keymap. */
    if (mm3 && key_down(VK_ESCAPE))
        pState->Gamepad.bAnalogButtons[XBOX_BUTTON_B] = 255;
    if (driving) {
        if (key_down(VK_SPACE)) pState->Gamepad.bAnalogButtons[XBOX_BUTTON_A] = 255;
        if (key_down('R')) pState->Gamepad.bAnalogButtons[XBOX_BUTTON_B] = 255;
        if (key_down('F')) pState->Gamepad.bAnalogButtons[XBOX_BUTTON_X] = 255;
        if (key_down('C')) pState->Gamepad.bAnalogButtons[XBOX_BUTTON_BLACK] = 255;
        if (key_down(VK_DOWN)) pState->Gamepad.bAnalogButtons[XBOX_BUTTON_LTRIGGER] = 255;
        if (key_down(VK_UP)) pState->Gamepad.bAnalogButtons[XBOX_BUTTON_RTRIGGER] = 255;
    }

    /* W/A/S/D would collide with the face buttons above, so the left thumb
     * shares the arrow keys' row on the numeric pad instead. */
    {
        BOOL left = bound_down(K_LX_NEG) || (driving && key_down(VK_LEFT));
        BOOL right = bound_down(K_LX_POS) || (driving && key_down(VK_RIGHT));
        pState->Gamepad.sThumbLX = (SHORT)((right - left) * 32767);
    }
    pState->Gamepad.sThumbLY = axis_from_keys(K_LY_NEG, K_LY_POS);
    pState->Gamepad.sThumbRX = axis_from_keys(K_RX_NEG, K_RX_POS);
    pState->Gamepad.sThumbRY = axis_from_keys(K_RY_NEG, K_RY_POS);

    /* The title's input layer looks for button edges, so the packet number
     * has to move whenever the state does or a press is never noticed. */
    pState->dwPacketNumber = ++packet;
}

/* RECOMP_PADMAP: which controller button drives each Xbox input, as
 * NAME:SOURCE,... (BLACK:RB,WHITE:LB). Xbox inputs and sources share these
 * names, except that the pad's shoulders are LB/RB and the Xbox's are
 * BLACK/WHITE; unnamed inputs keep their own button. A trigger used as a
 * button counts past a third of its travel. */
enum { PAD_SOURCES = 16 };
static const char *const k_pad_sources[PAD_SOURCES] = {
    "A", "B", "X", "Y", "LB", "RB", "START", "BACK", "LS", "RS",
    "UP", "DOWN", "LEFT", "RIGHT", "LT", "RT"};
static const char *const k_pad_inputs[PAD_SOURCES] = {
    "A", "B", "X", "Y", "BLACK", "WHITE", "START", "BACK", "LS", "RS",
    "UP", "DOWN", "LEFT", "RIGHT", "LT", "RT"};
static const WORD k_source_bits[14] = {
    XINPUT_GAMEPAD_A, XINPUT_GAMEPAD_B, XINPUT_GAMEPAD_X, XINPUT_GAMEPAD_Y,
    XINPUT_GAMEPAD_LEFT_SHOULDER, XINPUT_GAMEPAD_RIGHT_SHOULDER, XINPUT_GAMEPAD_START,
    XINPUT_GAMEPAD_BACK, XINPUT_GAMEPAD_LEFT_THUMB, XINPUT_GAMEPAD_RIGHT_THUMB,
    XINPUT_GAMEPAD_DPAD_UP, XINPUT_GAMEPAD_DPAD_DOWN, XINPUT_GAMEPAD_DPAD_LEFT,
    XINPUT_GAMEPAD_DPAD_RIGHT};

static const int *pad_map(void)
{
    static int map[PAD_SOURCES] = {-1};
    if (map[0] < 0) {
        const char *env = getenv("RECOMP_PADMAP");
        for (int i = 0; i < PAD_SOURCES; i++)
            map[i] = i;
        /* Match the Xbox Black/White positions and the SDL backend. */
        map[4] = 5; /* Black <- right shoulder */
        map[5] = 4; /* White <- left shoulder */
        while (env && *env) {
            char name[16], src[16];
            if (sscanf(env, "%15[^:]:%15[^,]", name, src) == 2)
                for (int i = 0; i < PAD_SOURCES; i++)
                    if (!_stricmp(name, k_pad_inputs[i]))
                        for (int j = 0; j < PAD_SOURCES; j++)
                            if (!_stricmp(src, k_pad_sources[j]))
                                map[i] = j;
            env = strchr(env, ',');
            env = env ? env + 1 : NULL;
        }
    }
    return map;
}

static BYTE pad_source(const XINPUT_GAMEPAD *g, int source)
{
    if (source == 14) return g->bLeftTrigger;
    if (source == 15) return g->bRightTrigger;
    return (g->wButtons & k_source_bits[source]) ? 255 : 0;
}

/* The Xbox buttons from an XInput pad, through RECOMP_PADMAP. */
static void pad_buttons(const XINPUT_GAMEPAD *g, XBOX_GAMEPAD *out)
{
    static const int analog[8] = {XBOX_BUTTON_A, XBOX_BUTTON_B, XBOX_BUTTON_X, XBOX_BUTTON_Y,
                                  XBOX_BUTTON_BLACK, XBOX_BUTTON_WHITE, -1, -1};
    static const WORD digital[8] = {XBOX_GAMEPAD_START, XBOX_GAMEPAD_BACK, XBOX_GAMEPAD_LEFT_THUMB,
                                    XBOX_GAMEPAD_RIGHT_THUMB, XBOX_GAMEPAD_DPAD_UP, XBOX_GAMEPAD_DPAD_DOWN,
                                    XBOX_GAMEPAD_DPAD_LEFT, XBOX_GAMEPAD_DPAD_RIGHT};
    const int *map = pad_map();
    out->wButtons = 0;
    for (int i = 0; i < 6; i++)
        out->bAnalogButtons[analog[i]] = pad_source(g, map[i]);
    for (int i = 0; i < 8; i++)
        if (pad_source(g, map[6 + i]) > 85)
            out->wButtons |= digital[i];
    out->bAnalogButtons[XBOX_BUTTON_LTRIGGER] = pad_source(g, map[14]);
    out->bAnalogButtons[XBOX_BUTTON_RTRIGGER] = pad_source(g, map[15]);
}

void xbox_InputInit(void)
{
    for (DWORD i = 0; i < XBOX_MAX_CONTROLLERS; i++) {
        XINPUT_STATE state;
        DWORD result = XInputGetState(i, &state);
        g_controller_connected[i] = (result == ERROR_SUCCESS);
    }
}

DWORD xbox_InputGetState(DWORD dwPort, XBOX_INPUT_STATE *pState)
{
    XINPUT_STATE xi_state;
    DWORD result;

    if (dwPort >= XBOX_MAX_CONTROLLERS || !pState)
        return ERROR_DEVICE_NOT_CONNECTED;

    if (xbox_FramebufferHostInputCaptured()) {
        DWORD connected = XInputGetState(dwPort, &xi_state);
        if (connected != ERROR_SUCCESS && !(dwPort == 0 && keyboard_enabled()))
            return connected;
        memset(pState, 0, sizeof *pState);
        /* Neutralization must be a new packet, even when the physical pad
         * has not changed. Keep subsequent physical packets in sequence. */
        ++g_packet_bias[dwPort];
        g_capture_active[dwPort] = TRUE;
        pState->dwPacketNumber = ++g_last_packet[dwPort];
        return ERROR_SUCCESS;
    }

    result = XInputGetState(dwPort, &xi_state);
    if (result != ERROR_SUCCESS) {
        g_controller_connected[dwPort] = FALSE;
        if (dwPort == 0 && keyboard_enabled()) {
            keyboard_state(pState);
            return finish_state(dwPort, pState);
        }
        return result;
    }

    g_controller_connected[dwPort] = TRUE;

    memset(pState, 0, sizeof(XBOX_INPUT_STATE));
    pState->dwPacketNumber = xi_state.dwPacketNumber;
    pad_buttons(&xi_state.Gamepad, &pState->Gamepad);

    pState->Gamepad.sThumbLX = xi_state.Gamepad.sThumbLX;
    pState->Gamepad.sThumbLY = xi_state.Gamepad.sThumbLY;
    pState->Gamepad.sThumbRX = xi_state.Gamepad.sThumbRX;
    pState->Gamepad.sThumbRY = xi_state.Gamepad.sThumbRY;

    if (dwPort == 0)
        keyboard_merge(pState);

    /* Mouse+keyboard merges on top of a real pad too: the mouse owns the
     * aim stick while it is moving, WASD owns movement while held. */
    if (dwPort == 0)
        mkb_merge(dwPort, pState);

    return finish_state(dwPort, pState);
}

DWORD xbox_InputSetState(DWORD dwPort, const XBOX_VIBRATION *pVibration)
{
    XINPUT_VIBRATION xi_vib;

    if (dwPort >= XBOX_MAX_CONTROLLERS || !pVibration)
        return ERROR_DEVICE_NOT_CONNECTED;

    xi_vib.wLeftMotorSpeed = pVibration->wLeftMotorSpeed;
    xi_vib.wRightMotorSpeed = pVibration->wRightMotorSpeed;
    return XInputSetState(dwPort, &xi_vib);
}

BOOL xbox_InputIsConnected(DWORD dwPort)
{
    if (dwPort >= XBOX_MAX_CONTROLLERS) return FALSE;
    return g_controller_connected[dwPort];
}

DWORD xbox_InputGetCapabilities(DWORD dwPort, DWORD dwFlags, XBOX_INPUT_CAPABILITIES *pCaps)
{
    XINPUT_CAPABILITIES xi_caps;
    DWORD result;

    if (dwPort >= XBOX_MAX_CONTROLLERS || !pCaps)
        return ERROR_DEVICE_NOT_CONNECTED;

    result = XInputGetCapabilities(dwPort, dwFlags, &xi_caps);
    if (result != ERROR_SUCCESS) return result;

    memset(pCaps, 0, sizeof(XBOX_INPUT_CAPABILITIES));
    pCaps->Type = xi_caps.Type;
    pCaps->SubType = xi_caps.SubType;
    pCaps->Flags = xi_caps.Flags;
    return ERROR_SUCCESS;
}

/* ======================================================================== */
#else /* !_WIN32 */
/* ====================  SDL2 GameController backend  ===================== */
/* ======================================================================== */

#include <SDL.h>

static SDL_GameController *g_pads[XBOX_MAX_CONTROLLERS];
static BOOL  g_controller_connected[XBOX_MAX_CONTROLLERS];
static DWORD g_packet[XBOX_MAX_CONTROLLERS];

/* Set while the framebuffer window's main thread pumps SDL events
 * (src/video/fb_present_sdl.c). The pump already updates the controllers,
 * and SDL_GameControllerUpdate pumps events itself, which on macOS must only
 * happen on the main thread; so then this side only reads them. */
static volatile int g_main_pump;

void xbox_InputSetMainPump(int on)
{
    g_main_pump = on;
}

/* Whether any controller is open: SDL must stay up for it at shutdown. */
int xbox_InputAnyOpen(void)
{
    for (int i = 0; i < XBOX_MAX_CONTROLLERS; i++)
        if (g_pads[i])
            return 1;
    return 0;
}

/* Open up to XBOX_MAX_CONTROLLERS attached game controllers. */
static void open_controllers(void)
{
    int slot = 0;
    for (int i = 0; i < SDL_NumJoysticks() && slot < XBOX_MAX_CONTROLLERS; i++) {
        if (!SDL_IsGameController(i))
            continue;
        if (!g_pads[slot]) {
            g_pads[slot] = SDL_GameControllerOpen(i);
            g_controller_connected[slot] = (g_pads[slot] != NULL);
        }
        slot++;
    }
}

void xbox_InputInit(void)
{
    /* Keep the default SIGINT/SIGTERM: SDL would turn them into an SDL_QUIT
     * that nothing reads without a window, and Ctrl-C would stop working. */
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
    if (!SDL_WasInit(SDL_INIT_GAMECONTROLLER))
        SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER);
    open_controllers();
}

DWORD xbox_InputGetState(DWORD dwPort, XBOX_INPUT_STATE *pState)
{
    if (dwPort >= XBOX_MAX_CONTROLLERS || !pState)
        return ERROR_DEVICE_NOT_CONNECTED;

    SDL_GameController *c = g_pads[dwPort];
    if (!c || !SDL_GameControllerGetAttached(c)) {
        g_controller_connected[dwPort] = FALSE;
        if (dwPort == 0 && xbox_InputKeyboardPoll(pState))
            return ERROR_SUCCESS;
        return ERROR_DEVICE_NOT_CONNECTED;
    }

    if (!g_main_pump)
        SDL_GameControllerUpdate();
    g_controller_connected[dwPort] = TRUE;

    memset(pState, 0, sizeof(XBOX_INPUT_STATE));
    pState->dwPacketNumber = ++g_packet[dwPort];

    WORD btn = 0;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_UP))    btn |= XBOX_GAMEPAD_DPAD_UP;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_DOWN))  btn |= XBOX_GAMEPAD_DPAD_DOWN;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_LEFT))  btn |= XBOX_GAMEPAD_DPAD_LEFT;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_RIGHT)) btn |= XBOX_GAMEPAD_DPAD_RIGHT;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_START))      btn |= XBOX_GAMEPAD_START;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_BACK))       btn |= XBOX_GAMEPAD_BACK;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_LEFTSTICK))  btn |= XBOX_GAMEPAD_LEFT_THUMB;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_RIGHTSTICK)) btn |= XBOX_GAMEPAD_RIGHT_THUMB;
    pState->Gamepad.wButtons = btn;

    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_A] =
        SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_A) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_B] =
        SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_B) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_X] =
        SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_X) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_Y] =
        SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_Y) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_BLACK] =
        SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_WHITE] =
        SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_LEFTSHOULDER) ? 255 : 0;

    /* SDL trigger axes are 0..32767 -> Xbox analog button 0..255 */
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_LTRIGGER] =
        (BYTE)(SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_TRIGGERLEFT) >> 7);
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_RTRIGGER] =
        (BYTE)(SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) >> 7);

    /* SDL Y axis points down; the Xbox Y axis points up -- invert.
     * Use (-1 - v) so v = -32768 does not overflow SHORT. */
    pState->Gamepad.sThumbLX = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTX);
    pState->Gamepad.sThumbLY =
        (SHORT)(-1 - SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTY));
    pState->Gamepad.sThumbRX = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_RIGHTX);
    pState->Gamepad.sThumbRY =
        (SHORT)(-1 - SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_RIGHTY));

    if (dwPort == 0)
        keyboard_merge(pState);
    return ERROR_SUCCESS;
}

DWORD xbox_InputSetState(DWORD dwPort, const XBOX_VIBRATION *pVibration)
{
    if (dwPort >= XBOX_MAX_CONTROLLERS || !pVibration)
        return ERROR_DEVICE_NOT_CONNECTED;

    SDL_GameController *c = g_pads[dwPort];
    if (!c) return ERROR_DEVICE_NOT_CONNECTED;

    /* SDL rumble needs a duration; refresh for ~1s on each call (the game
     * polls vibration continuously). */
    SDL_GameControllerRumble(c, pVibration->wLeftMotorSpeed,
                             pVibration->wRightMotorSpeed, 1000);
    return ERROR_SUCCESS;
}

BOOL xbox_InputIsConnected(DWORD dwPort)
{
    if (dwPort >= XBOX_MAX_CONTROLLERS) return FALSE;
    return g_controller_connected[dwPort];
}

DWORD xbox_InputGetCapabilities(DWORD dwPort, DWORD dwFlags, XBOX_INPUT_CAPABILITIES *pCaps)
{
    (void)dwFlags;
    if (dwPort >= XBOX_MAX_CONTROLLERS || !pCaps)
        return ERROR_DEVICE_NOT_CONNECTED;
    if (!g_pads[dwPort])
        return ERROR_DEVICE_NOT_CONNECTED;

    memset(pCaps, 0, sizeof(XBOX_INPUT_CAPABILITIES));
    pCaps->Type    = 1;   /* XINPUT_DEVTYPE_GAMEPAD */
    pCaps->SubType = 1;   /* XINPUT_DEVSUBTYPE_GAMEPAD */
    pCaps->Flags   = 0;
    return ERROR_SUCCESS;
}

#endif /* _WIN32 */
