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

/* Configure before input polling starts. The optional probe runs on the
 * input thread, so it must be fast and safe to call from that thread.
 * For the MM3 keyboard layout, TRUE maps arrows to driving controls;
 * FALSE (or no probe) maps them only to the menu d-pad. Legacy is unchanged. */
typedef BOOL (*XBOX_KEYBOARD_DRIVING_PROBE)(void);
void xbox_InputSetKeyboardDrivingProbe(XBOX_KEYBOARD_DRIVING_PROBE probe);

/**
 * Get the state of a controller.
 * Port: 0-3 (Xbox controller ports)
 */
DWORD xbox_InputGetState(DWORD dwPort, XBOX_INPUT_STATE *pState);

/* RECOMP_MENU_KEYS applies only while in_menu() returns nonzero (Windows). */
void xbox_InputSetMenuProbe(int (*in_menu)(void));
/* Menu pointing: pointer(x, y) for a client-area fraction returns 0 off any
 * item, 1 on one (*offset rows below the focused item, negative above), -1
 * on one whose focus cannot be told yet. Without it, clicks are just A/B. */
void xbox_InputSetMenuPointer(int (*pointer)(float x, float y, int *offset));

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

#ifdef __cplusplus
}
#endif

#endif /* BURNOUT3_XINPUT_XBOX_H */
