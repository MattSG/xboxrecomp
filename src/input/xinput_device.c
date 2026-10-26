/**
 * Xbox Input Compatibility Layer
 *
 * Translates the Xbox controller API to a host gamepad backend.
 * Handles the structural differences between the Xbox gamepad
 * (analog buttons as bytes, separate trigger channels) and the host
 * (digital face buttons, trigger axes).
 *
 * The keyboard (keyboard.c), mouse (mkb_input.c), RECOMP_PADMAP and packet
 * logic are shared; only reading
 * the host pad differs:
 *   _WIN32 -> Windows XInput
 *   other  -> SDL3 gamepads, read into the same XINPUT_STATE
 */

#include "xinput_xbox.h"
#include "keyboard.h"
#include "mkb_input.h"
#if !defined(_WIN32)
/* The mouse layer reads Win32 raw input (mkb_input.c); elsewhere it is off. */
#define mkb_enabled() 0
#define mkb_merge(port, state) 0
#endif
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

static XBOX_KEYBOARD_HOOK s_keyboard_hook;

void xbox_InputSetKeyboardHook(XBOX_KEYBOARD_HOOK hook)
{
    s_keyboard_hook = hook;
}

#if defined(_WIN32)
#include <xinput.h>
#pragma comment(lib, "xinput.lib")
#define host_pad_state XInputGetState
#else
#include <SDL3/SDL.h>

/* SDL3 gamepads as XInput ports: port n is the n-th gamepad SDL lists. The
 * framebuffer window's thread pumps SDL events, which keeps the state
 * current; SDL locks its joystick state, so reading it here is safe. */
static SDL_Gamepad *g_sdl_pads[XBOX_MAX_CONTROLLERS];
static XINPUT_GAMEPAD g_sdl_last[XBOX_MAX_CONTROLLERS];
static DWORD g_sdl_packet[XBOX_MAX_CONTROLLERS];

static SDL_Gamepad *sdl_pad(DWORD port)
{
    static Uint64 next_scan[XBOX_MAX_CONTROLLERS];
    SDL_Gamepad *pad = g_sdl_pads[port];
    if (pad && SDL_GamepadConnected(pad))
        return pad;
    if (pad) {
        SDL_CloseGamepad(pad);
        g_sdl_pads[port] = NULL;
    }
    if (SDL_GetTicks() < next_scan[port])
        return NULL;
    next_scan[port] = SDL_GetTicks() + 1000; /* hot-plug check, not every poll */
    {
        int count = 0;
        SDL_JoystickID *ids = SDL_GetGamepads(&count);
        if (ids && (int)port < count)
            g_sdl_pads[port] = SDL_OpenGamepad(ids[port]);
        SDL_free(ids);
    }
    return g_sdl_pads[port];
}

static DWORD host_pad_state(DWORD port, XINPUT_STATE *state)
{
    static const struct { SDL_GamepadButton b; WORD bit; } k_buttons[] = {
        { SDL_GAMEPAD_BUTTON_DPAD_UP, XINPUT_GAMEPAD_DPAD_UP },
        { SDL_GAMEPAD_BUTTON_DPAD_DOWN, XINPUT_GAMEPAD_DPAD_DOWN },
        { SDL_GAMEPAD_BUTTON_DPAD_LEFT, XINPUT_GAMEPAD_DPAD_LEFT },
        { SDL_GAMEPAD_BUTTON_DPAD_RIGHT, XINPUT_GAMEPAD_DPAD_RIGHT },
        { SDL_GAMEPAD_BUTTON_START, XINPUT_GAMEPAD_START },
        { SDL_GAMEPAD_BUTTON_BACK, XINPUT_GAMEPAD_BACK },
        { SDL_GAMEPAD_BUTTON_LEFT_STICK, XINPUT_GAMEPAD_LEFT_THUMB },
        { SDL_GAMEPAD_BUTTON_RIGHT_STICK, XINPUT_GAMEPAD_RIGHT_THUMB },
        { SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, XINPUT_GAMEPAD_LEFT_SHOULDER },
        { SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, XINPUT_GAMEPAD_RIGHT_SHOULDER },
        { SDL_GAMEPAD_BUTTON_SOUTH, XINPUT_GAMEPAD_A },
        { SDL_GAMEPAD_BUTTON_EAST, XINPUT_GAMEPAD_B },
        { SDL_GAMEPAD_BUTTON_WEST, XINPUT_GAMEPAD_X },
        { SDL_GAMEPAD_BUTTON_NORTH, XINPUT_GAMEPAD_Y } };
    SDL_Gamepad *pad = sdl_pad(port);
    XINPUT_GAMEPAD *g = &state->Gamepad;
    size_t i;

    if (!pad)
        return ERROR_DEVICE_NOT_CONNECTED;
    memset(state, 0, sizeof *state);
    for (i = 0; i < sizeof k_buttons / sizeof k_buttons[0]; i++)
        if (SDL_GetGamepadButton(pad, k_buttons[i].b))
            g->wButtons |= k_buttons[i].bit;
    /* Triggers are 0..32767; XInput's are 0..255. */
    g->bLeftTrigger = (BYTE)(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) >> 7);
    g->bRightTrigger = (BYTE)(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) >> 7);
    /* SDL's Y axes point down, XInput's up; -1 - v keeps -32768 in range. */
    g->sThumbLX = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX);
    g->sThumbLY = (SHORT)(-1 - SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY));
    g->sThumbRX = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHTX);
    g->sThumbRY = (SHORT)(-1 - SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHTY));
    /* XInput's packet number moves only when the state does. */
    if (memcmp(g, &g_sdl_last[port], sizeof *g)) {
        g_sdl_last[port] = *g;
        ++g_sdl_packet[port];
    }
    state->dwPacketNumber = g_sdl_packet[port];
    return ERROR_SUCCESS;
}
#endif

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

/* ---- keyboard and mouse on port 0 ----------------------------------------
 *
 * The keys come from keyboard.c (RECOMP_KEYBOARD=1, the map, RECOMP_KEYMAP
 * and RECOMP_MENU_KEYS); the windows record them. Here, beside the pad: a
 * menu click and the title's contextual keys (xbox_InputSetKeyboardHook).
 *
 * Declared rather than included so src/input does not depend on the video
 * module's include path. */
extern int xbox_FramebufferHostInputCaptured(void);
extern int xbox_FramebufferMouse(float *x, float *y);
extern void xbox_FramebufferSetMousePressHandler(int (*handler)(float, float));

/* Optional title click dispatch, with mouse-to-A fallback. */
static int (*s_menu_click)(float x, float y);

static BOOL keyboard_enabled(void)
{
    static int on = -1;
    if (on < 0) {
        const char *v = getenv("RECOMP_KEYBOARD");
        on = (v && *v && *v != '0') ? 1 : 0;
    }
    return on ? TRUE : FALSE;
}

static int menu_press(float x, float y)
{
    return keyboard_enabled() && xbox_InputKeyboardMenu() &&
        !xbox_FramebufferHostInputCaptured() && s_menu_click && s_menu_click(x, y);
}

void xbox_InputSetMenuClickHandler(int (*click)(float, float))
{
    s_menu_click = click;
    xbox_FramebufferSetMousePressHandler(click ? menu_press : NULL);
}

/* A left click the title did not take is a short press of A. */
static BOOL menu_click(void)
{
    static int was_down;
    static ULONGLONG a_until;
    ULONGLONG now = GetTickCount64();
    float x, y;
    int down = xbox_FramebufferKeyDown(XBOX_VK_LBUTTON);
    xbox_FramebufferMouse(&x, &y);
    if (down && !was_down && !(s_menu_click && s_menu_click(x, y)))
        a_until = now + 80;
    was_down = down;
    return now < a_until;
}

/* The keyboard's pad, or 0 with RECOMP_KEYBOARD off. */
static int keyboard_pad(XBOX_INPUT_STATE *s)
{
    int menu;
    if (!xbox_InputKeyboardPoll(s))
        return 0;
    menu = xbox_InputKeyboardMenu();
    if (menu && menu_click())
        s->Gamepad.bAnalogButtons[XBOX_BUTTON_A] = 255;
    if (s_keyboard_hook)
        s_keyboard_hook(s, menu ? TRUE : FALSE);
    return 1;
}

/* Merge the keyboard on top of a pad on port 0 rather than only standing in
 * for a missing one.
 *
 * The first version put the keyboard behind XInput's failure, on the
 * assumption that with nothing plugged in the call would fail. It does
 * not: under Wine XInputGetState returns ERROR_SUCCESS and a gamepad
 * with every button at rest, so the fallback was unreachable and
 * pressing a key did nothing at all.
 *
 * Merging is also the better rule. A real pad keeps working -- its
 * buttons are already in pState and the keyboard only adds to them --
 * and there is no special case left to get wrong. */
static void keyboard_merge(XBOX_INPUT_STATE *pState)
{
    XBOX_INPUT_STATE kb;
    int i;

    if (!keyboard_pad(&kb))
        return;
    pState->Gamepad.wButtons |= kb.Gamepad.wButtons;
    for (i = 0; i < 8; i++)
        if (kb.Gamepad.bAnalogButtons[i] > pState->Gamepad.bAnalogButtons[i])
            pState->Gamepad.bAnalogButtons[i] = kb.Gamepad.bAnalogButtons[i];
    if (kb.Gamepad.sThumbLX) pState->Gamepad.sThumbLX = kb.Gamepad.sThumbLX;
    if (kb.Gamepad.sThumbLY) pState->Gamepad.sThumbLY = kb.Gamepad.sThumbLY;
    if (kb.Gamepad.sThumbRX) pState->Gamepad.sThumbRX = kb.Gamepad.sThumbRX;
    if (kb.Gamepad.sThumbRY) pState->Gamepad.sThumbRY = kb.Gamepad.sThumbRY;
    /* The input layer records edges, so an unchanged packet number is
     * read as the same state and the press never happens. */
    pState->dwPacketNumber = kb.dwPacketNumber;
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
#if !defined(_WIN32)
    if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD))
        fprintf(stderr, "[INPUT] SDL gamepad init failed: %s\n", SDL_GetError());
#endif
    for (DWORD i = 0; i < XBOX_MAX_CONTROLLERS; i++) {
        XINPUT_STATE state;
        DWORD result = host_pad_state(i, &state);
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
        DWORD connected = host_pad_state(dwPort, &xi_state);
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

    result = host_pad_state(dwPort, &xi_state);
    if (result != ERROR_SUCCESS) {
        g_controller_connected[dwPort] = FALSE;
        if (dwPort == 0) {
            /* Keyboard and mouse stand in for an absent pad, so the game is
             * playable with no controller plugged in. */
            int kb;
            memset(pState, 0, sizeof *pState);
            kb = keyboard_pad(pState);
            if (mkb_merge(dwPort, pState))
                g_controller_connected[dwPort] = TRUE;
            if (kb || mkb_enabled())
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

    if (dwPort == 0) {
        keyboard_merge(pState);
        /* Mouse+keyboard merges on top of a real pad too: the mouse owns the
         * aim stick while it is moving, WASD owns movement while held. */
        mkb_merge(dwPort, pState);
    }

    return finish_state(dwPort, pState);
}

DWORD xbox_InputSetState(DWORD dwPort, const XBOX_VIBRATION *pVibration)
{
    if (dwPort >= XBOX_MAX_CONTROLLERS || !pVibration)
        return ERROR_DEVICE_NOT_CONNECTED;
#if defined(_WIN32)
    {
        XINPUT_VIBRATION xi_vib;
        xi_vib.wLeftMotorSpeed = pVibration->wLeftMotorSpeed;
        xi_vib.wRightMotorSpeed = pVibration->wRightMotorSpeed;
        return XInputSetState(dwPort, &xi_vib);
    }
#else
    {
        SDL_Gamepad *pad = sdl_pad(dwPort);
        if (!pad)
            return ERROR_DEVICE_NOT_CONNECTED;
        /* SDL rumble runs for a duration; titles resend it continuously. */
        SDL_RumbleGamepad(pad, pVibration->wLeftMotorSpeed, pVibration->wRightMotorSpeed, 1000);
        return ERROR_SUCCESS;
    }
#endif
}

BOOL xbox_InputIsConnected(DWORD dwPort)
{
    if (dwPort >= XBOX_MAX_CONTROLLERS) return FALSE;
    return g_controller_connected[dwPort];
}

DWORD xbox_InputGetCapabilities(DWORD dwPort, DWORD dwFlags, XBOX_INPUT_CAPABILITIES *pCaps)
{
    if (dwPort >= XBOX_MAX_CONTROLLERS || !pCaps)
        return ERROR_DEVICE_NOT_CONNECTED;
#if defined(_WIN32)
    {
        XINPUT_CAPABILITIES xi_caps;
        DWORD result = XInputGetCapabilities(dwPort, dwFlags, &xi_caps);
        if (result != ERROR_SUCCESS) return result;
        memset(pCaps, 0, sizeof(XBOX_INPUT_CAPABILITIES));
        pCaps->Type = xi_caps.Type;
        pCaps->SubType = xi_caps.SubType;
        pCaps->Flags = xi_caps.Flags;
    }
#else
    (void)dwFlags;
    if (!sdl_pad(dwPort))
        return ERROR_DEVICE_NOT_CONNECTED;
    memset(pCaps, 0, sizeof(XBOX_INPUT_CAPABILITIES));
    pCaps->Type = 1;      /* XINPUT_DEVTYPE_GAMEPAD */
    pCaps->SubType = 1;   /* XINPUT_DEVSUBTYPE_GAMEPAD */
#endif
    return ERROR_SUCCESS;
}
