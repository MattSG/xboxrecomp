/**
 * Xbox Input Compatibility Layer - Type Definitions
 *
 * Defines the Xbox XPP (Xbox Peripheral Port) input types and
 * translates them to Windows XInput. The Xbox controller API
 * differs from XInput in structure layout and polling model.
 *
 * The game uses 9 XPP entry points with 12 total calls:
 * - Controller state polling
 * - Vibration/force feedback
 * - Device enumeration
 */

#ifndef BURNOUT3_XINPUT_XBOX_H
#define BURNOUT3_XINPUT_XBOX_H

#include <stdint.h>
#include "platform/xbox_winnt.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * Xbox input types
 * ================================================================ */

/* Xbox controller port count */
#define XBOX_MAX_CONTROLLERS 4

/* Xbox gamepad button flags */
#define XBOX_GAMEPAD_DPAD_UP        0x0001
#define XBOX_GAMEPAD_DPAD_DOWN      0x0002
#define XBOX_GAMEPAD_DPAD_LEFT      0x0004
#define XBOX_GAMEPAD_DPAD_RIGHT     0x0008
#define XBOX_GAMEPAD_START          0x0010
#define XBOX_GAMEPAD_BACK           0x0020
#define XBOX_GAMEPAD_LEFT_THUMB     0x0040
#define XBOX_GAMEPAD_RIGHT_THUMB    0x0080

/* Xbox analog button thresholds */
#define XBOX_ANALOG_BUTTON_THRESHOLD 30

typedef struct XBOX_GAMEPAD {
    WORD  wButtons;             /* Digital button bitmask */
    BYTE  bAnalogButtons[8];    /* A, B, X, Y, Black, White, LTrig, RTrig (0-255) */
    SHORT sThumbLX;             /* Left stick X (-32768 to 32767) */
    SHORT sThumbLY;             /* Left stick Y */
    SHORT sThumbRX;             /* Right stick X */
    SHORT sThumbRY;             /* Right stick Y */
} XBOX_GAMEPAD;

typedef struct XBOX_INPUT_STATE {
    DWORD dwPacketNumber;
    XBOX_GAMEPAD Gamepad;
} XBOX_INPUT_STATE;

typedef struct XBOX_VIBRATION {
    WORD wLeftMotorSpeed;       /* Low-frequency rumble (0-65535) */
    WORD wRightMotorSpeed;      /* High-frequency rumble (0-65535) */
} XBOX_VIBRATION;

typedef struct XBOX_INPUT_CAPABILITIES {
    BYTE Type;
    BYTE SubType;
    WORD Flags;
    XBOX_GAMEPAD Gamepad;
    XBOX_VIBRATION Vibration;
} XBOX_INPUT_CAPABILITIES;

/* Analog button indices */
#define XBOX_BUTTON_A           0
#define XBOX_BUTTON_B           1
#define XBOX_BUTTON_X           2
#define XBOX_BUTTON_Y           3
#define XBOX_BUTTON_BLACK       4
#define XBOX_BUTTON_WHITE       5
#define XBOX_BUTTON_LTRIGGER    6
#define XBOX_BUTTON_RTRIGGER    7

/* ================================================================
 * Public API
 * ================================================================ */

/**
 * Initialize the input system.
 * Maps Xbox controller ports to XInput slots.
 */
void xbox_InputInit(void);

/**
 * Get the state of a controller.
 * Port: 0-3 (Xbox controller ports)
 */
DWORD xbox_InputGetState(DWORD dwPort, XBOX_INPUT_STATE *pState);

/**
 * Set controller vibration.
 */
DWORD xbox_InputSetState(DWORD dwPort, const XBOX_VIBRATION *pVibration);

/**
 * Check if a controller is connected.
 */
BOOL xbox_InputIsConnected(DWORD dwPort);

/**
 * Get controller capabilities.
 */
DWORD xbox_InputGetCapabilities(DWORD dwPort, DWORD dwFlags, XBOX_INPUT_CAPABILITIES *pCaps);

/* ---- Hooks ------------------------------------------------------------- */

/* A port's contextual keys (RECOMP_KEYBOARD=1): the hook may change the
 * keyboard pad after the default keys are read. `menu` is TRUE while
 * RECOMP_MENU_KEYS navigation applies; xbox_FramebufferKeyDown reads a key.
 * Configure before input polling starts. It runs on the input thread, so it
 * must be fast and safe to call from that thread. */
typedef void (*XBOX_KEYBOARD_HOOK)(XBOX_INPUT_STATE *state, BOOL menu);
void xbox_InputSetKeyboardHook(XBOX_KEYBOARD_HOOK hook);

/* RECOMP_MENU_KEYS applies only while in_menu() returns nonzero. */
void xbox_InputSetMenuProbe(int (*in_menu)(void));

/* Register before polling. A nonzero result consumes a menu click so the
 * title can dispatch it on its guest thread instead of receiving pad A. */
void xbox_InputSetMenuClickHandler(int (*click)(float x, float y));

/* The raw host pad, supplied by the embedding project instead of XInput (an
 * SDL3 gamepad layer, say). XInput's layout: `buttons` are the
 * XINPUT_GAMEPAD_* bits, triggers 0-255, sticks up-positive. `packet` must
 * change whenever the state does. The toolkit maps it to the Xbox pad
 * (RECOMP_PADMAP) and adds the keyboard and mouse, UI-capture neutralisation
 * and packet sequencing, so every backend behaves the same. */
typedef struct {
    uint16_t buttons;
    uint8_t  left_trigger, right_trigger;
    int16_t  lx, ly, rx, ry;
    uint32_t packet;
} XBOX_HOST_PAD;

/* Register before xbox_InputInit; NULL restores XInput. The provider is
 * borrowed and must stay valid. `get` fills the pad and returns ERROR_SUCCESS,
 * or ERROR_DEVICE_NOT_CONNECTED (1167) for an empty port; `rumble` and `init`
 * are optional. May be called from any thread the title polls on. */
typedef struct {
    void  (*init)(void);
    DWORD (*get)(DWORD port, XBOX_HOST_PAD *pad);
    void  (*rumble)(DWORD port, WORD left, WORD right);
} XBOX_HOST_PAD_PROVIDER;
void xbox_InputSetHostPadProvider(const XBOX_HOST_PAD_PROVIDER *provider);

/* The host pad on a port in XInput's layout (wButtons are the XINPUT_GAMEPAD_*
 * bits, triggers 0-255, sticks up-positive), without the keyboard and mouse the
 * title's state merges in. For launchers and overlays that need the raw pad.
 * Returns 0 when read, ERROR_DEVICE_NOT_CONNECTED (1167) otherwise. */
uint32_t xbox_InputHostPad(uint32_t port, uint16_t *buttons,
                           uint8_t *left_trigger, uint8_t *right_trigger,
                           int16_t *lx, int16_t *ly, int16_t *rx, int16_t *ry);


#if !defined(_WIN32)
/* The POSIX SDL presenter owns event pumping while its main-thread window is
 * active, and keeps SDL alive at shutdown while controller handles are open. */
void xbox_InputSetMainPump(int on);
int xbox_InputAnyOpen(void);
#endif

#ifdef __cplusplus
}
#endif

#endif /* BURNOUT3_XINPUT_XBOX_H */
