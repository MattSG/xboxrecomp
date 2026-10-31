/**
 * Xbox Input Compatibility Layer
 *
 * Translates the Xbox controller API to SDL3 gamepads on every host.
 * Handles the structural differences between the Xbox gamepad
 * (analog buttons as bytes, separate trigger channels) and the host
 * (digital face buttons, trigger axes).
 */

#include "xinput_xbox.h"
#include <SDL3/SDL.h>
#include <stdlib.h>
#include <string.h>

static SDL_Gamepad *g_pads[XBOX_MAX_CONTROLLERS];
static BOOL  g_controller_connected[XBOX_MAX_CONTROLLERS];
static DWORD g_packet[XBOX_MAX_CONTROLLERS];

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

/* The framebuffer window records what is held; see src/video/fb_present.c.
 * The argument is a Win32 virtual-key code, which is also what the window
 * layer reports, so this file needs nothing from the window module's headers.
 * Declared rather than included so src/input does not depend on the video
 * module's include path. */
extern int xbox_FramebufferKeyDown(int vk);

#define KB_VK_BACK    0x08
#define KB_VK_RETURN  0x0D
#define KB_VK_SHIFT   0x10
#define KB_VK_CONTROL 0x11
#define KB_VK_LEFT    0x25
#define KB_VK_UP      0x26
#define KB_VK_RIGHT   0x27
#define KB_VK_DOWN    0x28
#define KB_VK_NUMPAD2 0x62
#define KB_VK_NUMPAD4 0x64
#define KB_VK_NUMPAD6 0x66
#define KB_VK_NUMPAD8 0x68

static BOOL key_down(int vk)
{
    return xbox_FramebufferKeyDown(vk) ? TRUE : FALSE;
}

/* A thumb axis from two keys, at the full deflection a digital key implies. */
static SHORT axis_from_keys(int negative, int positive)
{
    int v = 0;
    if (key_down(negative)) v -= 32767;
    if (key_down(positive)) v += 32767;
    return (SHORT)v;
}

static void keyboard_state(XBOX_INPUT_STATE *pState)
{
    static DWORD packet;
    WORD b = 0;

    memset(pState, 0, sizeof(*pState));

    if (key_down(KB_VK_UP))      b |= XBOX_GAMEPAD_DPAD_UP;
    if (key_down(KB_VK_DOWN))    b |= XBOX_GAMEPAD_DPAD_DOWN;
    if (key_down(KB_VK_LEFT))    b |= XBOX_GAMEPAD_DPAD_LEFT;
    if (key_down(KB_VK_RIGHT))   b |= XBOX_GAMEPAD_DPAD_RIGHT;
    if (key_down(KB_VK_RETURN))  b |= XBOX_GAMEPAD_START;
    if (key_down(KB_VK_BACK))    b |= XBOX_GAMEPAD_BACK;
    if (key_down(KB_VK_SHIFT))   b |= XBOX_GAMEPAD_LEFT_THUMB;
    if (key_down(KB_VK_CONTROL)) b |= XBOX_GAMEPAD_RIGHT_THUMB;
    pState->Gamepad.wButtons = b;

    /* Analog on the console, so a key is 255 rather than a flag -- a title
     * that reads these as a pressure never sees a press if they are 1. */
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_A]        = key_down('Z') ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_B]        = key_down('X') ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_X]        = key_down('A') ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_Y]        = key_down('S') ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_WHITE]    = key_down('Q') ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_BLACK]    = key_down('E') ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_LTRIGGER] = key_down('1') ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_RTRIGGER] = key_down('3') ? 255 : 0;

    /* W/A/S/D would collide with the face buttons above, so the left thumb
     * shares the arrow keys' row on the numeric pad instead. */
    pState->Gamepad.sThumbLX = axis_from_keys(KB_VK_NUMPAD4, KB_VK_NUMPAD6);
    pState->Gamepad.sThumbLY = axis_from_keys(KB_VK_NUMPAD2, KB_VK_NUMPAD8);
    pState->Gamepad.sThumbRX = axis_from_keys('J', 'L');
    pState->Gamepad.sThumbRY = axis_from_keys('K', 'I');

    /* The title's input layer looks for button edges, so the packet number
     * has to move whenever the state does or a press is never noticed. */
    pState->dwPacketNumber = ++packet;
}

/* Open up to XBOX_MAX_CONTROLLERS attached gamepads. */
static void open_controllers(void)
{
    int count = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&count);
    int slot = 0;

    if (!ids)
        return;
    for (int i = 0; i < count && slot < XBOX_MAX_CONTROLLERS; i++) {
        if (!g_pads[slot]) {
            g_pads[slot] = SDL_OpenGamepad(ids[i]);
            g_controller_connected[slot] = (g_pads[slot] != NULL);
        }
        slot++;
    }
    SDL_free(ids);
}

void xbox_InputInit(void)
{
    if (!SDL_WasInit(SDL_INIT_GAMEPAD))
        SDL_InitSubSystem(SDL_INIT_GAMEPAD);
    open_controllers();
}

DWORD xbox_InputGetState(DWORD dwPort, XBOX_INPUT_STATE *pState)
{
    SDL_Gamepad *c;
    WORD btn = 0;

    if (dwPort >= XBOX_MAX_CONTROLLERS || !pState)
        return ERROR_DEVICE_NOT_CONNECTED;

    c = g_pads[dwPort];
    if (!c || !SDL_GamepadConnected(c)) {
        g_controller_connected[dwPort] = FALSE;
        if (dwPort == 0 && keyboard_enabled()) {
            keyboard_state(pState);
            return ERROR_SUCCESS;
        }
        return ERROR_DEVICE_NOT_CONNECTED;
    }

    SDL_UpdateGamepads();
    g_controller_connected[dwPort] = TRUE;

    memset(pState, 0, sizeof(XBOX_INPUT_STATE));
    pState->dwPacketNumber = ++g_packet[dwPort];

    if (SDL_GetGamepadButton(c, SDL_GAMEPAD_BUTTON_DPAD_UP))    btn |= XBOX_GAMEPAD_DPAD_UP;
    if (SDL_GetGamepadButton(c, SDL_GAMEPAD_BUTTON_DPAD_DOWN))  btn |= XBOX_GAMEPAD_DPAD_DOWN;
    if (SDL_GetGamepadButton(c, SDL_GAMEPAD_BUTTON_DPAD_LEFT))  btn |= XBOX_GAMEPAD_DPAD_LEFT;
    if (SDL_GetGamepadButton(c, SDL_GAMEPAD_BUTTON_DPAD_RIGHT)) btn |= XBOX_GAMEPAD_DPAD_RIGHT;
    if (SDL_GetGamepadButton(c, SDL_GAMEPAD_BUTTON_START))      btn |= XBOX_GAMEPAD_START;
    if (SDL_GetGamepadButton(c, SDL_GAMEPAD_BUTTON_BACK))       btn |= XBOX_GAMEPAD_BACK;
    if (SDL_GetGamepadButton(c, SDL_GAMEPAD_BUTTON_LEFT_STICK)) btn |= XBOX_GAMEPAD_LEFT_THUMB;
    if (SDL_GetGamepadButton(c, SDL_GAMEPAD_BUTTON_RIGHT_STICK))btn |= XBOX_GAMEPAD_RIGHT_THUMB;
    pState->Gamepad.wButtons = btn;

    /* SDL names face buttons by position, not label: SOUTH is the bottom
     * button on any pad, which is A on an Xbox layout. */
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_A] =
        SDL_GetGamepadButton(c, SDL_GAMEPAD_BUTTON_SOUTH) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_B] =
        SDL_GetGamepadButton(c, SDL_GAMEPAD_BUTTON_EAST) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_X] =
        SDL_GetGamepadButton(c, SDL_GAMEPAD_BUTTON_WEST) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_Y] =
        SDL_GetGamepadButton(c, SDL_GAMEPAD_BUTTON_NORTH) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_BLACK] =
        SDL_GetGamepadButton(c, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_WHITE] =
        SDL_GetGamepadButton(c, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER) ? 255 : 0;

    /* SDL trigger axes are 0..32767 -> Xbox analog button 0..255 */
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_LTRIGGER] =
        (BYTE)(SDL_GetGamepadAxis(c, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) >> 7);
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_RTRIGGER] =
        (BYTE)(SDL_GetGamepadAxis(c, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) >> 7);

    /* SDL Y axis points down; the Xbox Y axis points up -- invert.
     * Use (-1 - v) so v = -32768 does not overflow SHORT. */
    pState->Gamepad.sThumbLX = SDL_GetGamepadAxis(c, SDL_GAMEPAD_AXIS_LEFTX);
    pState->Gamepad.sThumbLY =
        (SHORT)(-1 - SDL_GetGamepadAxis(c, SDL_GAMEPAD_AXIS_LEFTY));
    pState->Gamepad.sThumbRX = SDL_GetGamepadAxis(c, SDL_GAMEPAD_AXIS_RIGHTX);
    pState->Gamepad.sThumbRY =
        (SHORT)(-1 - SDL_GetGamepadAxis(c, SDL_GAMEPAD_AXIS_RIGHTY));

    /* Merge the keyboard on top rather than only standing in for a missing
     * pad: a real pad keeps working, and a host whose pad API reports a
     * connected device at rest (Wine's XInput did) still answers keys. */
    if (dwPort == 0 && keyboard_enabled()) {
        XBOX_INPUT_STATE kb;
        int i;
        keyboard_state(&kb);
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

    return ERROR_SUCCESS;
}

DWORD xbox_InputSetState(DWORD dwPort, const XBOX_VIBRATION *pVibration)
{
    SDL_Gamepad *c;

    if (dwPort >= XBOX_MAX_CONTROLLERS || !pVibration)
        return ERROR_DEVICE_NOT_CONNECTED;

    c = g_pads[dwPort];
    if (!c) return ERROR_DEVICE_NOT_CONNECTED;

    /* SDL rumble needs a duration; refresh for ~1s on each call (the game
     * polls vibration continuously). */
    SDL_RumbleGamepad(c, pVibration->wLeftMotorSpeed,
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
