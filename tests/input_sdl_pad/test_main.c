/* The production SDL3 pad path against an SDL virtual gamepad: button and stick mapping into the
 * XInput layout, triggers, rumble reaching the device, and unplug / replug. No controller needed. */
#include <windows.h>
#include <xinput.h>
#include <SDL3/SDL.h>
#include <stdio.h>
#include <string.h>
#include "input/xinput_device.c"

/* The framebuffer window is outside this test: no keys, no mouse, no capture. */
int xbox_FramebufferHostInputCaptured(void) { return 0; }
int xbox_FramebufferMouse(float *x, float *y) { *x = *y = 0; return 0; }
void xbox_FramebufferSetMousePressHandler(int (*handler)(float, float)) { (void)handler; }

static unsigned checks, failures;
#define CHECK(c) do { ++checks; if (!(c)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)

static Uint16 rumble_low, rumble_high;
static bool rumble(void *user, Uint16 low, Uint16 high) { (void)user; rumble_low = low; rumble_high = high; return true; }

static SDL_Joystick *attach(SDL_JoystickID *id)
{
    SDL_VirtualJoystickDesc desc;
    SDL_INIT_INTERFACE(&desc);
    desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
    desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    desc.Rumble = rumble;
    *id = SDL_AttachVirtualJoystick(&desc);
    return *id ? SDL_OpenJoystick(*id) : NULL;
}

/* A pad appears (or goes) within the 1 s hot-plug rescan. */
static int wait_present(int want)
{
    uint16_t b;
    for (int i = 0; i < 40; ++i) {
        if ((xbox_InputHostPad(0, &b, 0, 0, 0, 0, 0, 0) == 0) == want) return 1;
        SDL_Delay(100);
    }
    return 0;
}

int main(void)
{
    SDL_JoystickID id;
    SDL_Joystick *js;
    uint16_t buttons;
    uint8_t lt, rt;
    int16_t lx, ly, rx, ry;
    XBOX_VIBRATION vib = { 0x8000, 0x4000 };

    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    CHECK(SDL_Init(SDL_INIT_GAMEPAD));
    CHECK(xbox_InputHostPad(0, &buttons, &lt, &rt, &lx, &ly, &rx, &ry) == ERROR_DEVICE_NOT_CONNECTED);

    js = attach(&id);
    CHECK(js != NULL);
    CHECK(wait_present(1));

    /* Buttons: SDL's positional south/east/west/north are XInput A/B/X/Y, shoulders and start map across. */
    SDL_SetJoystickVirtualButton(js, SDL_GAMEPAD_BUTTON_SOUTH, true);
    SDL_SetJoystickVirtualButton(js, SDL_GAMEPAD_BUTTON_START, true);
    SDL_SetJoystickVirtualButton(js, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, true);
    SDL_SetJoystickVirtualButton(js, SDL_GAMEPAD_BUTTON_DPAD_UP, true);
    SDL_Delay(30);
    CHECK(xbox_InputHostPad(0, &buttons, &lt, &rt, &lx, &ly, &rx, &ry) == 0);
    CHECK(buttons == (XINPUT_GAMEPAD_A | XINPUT_GAMEPAD_START | XINPUT_GAMEPAD_LEFT_SHOULDER | XINPUT_GAMEPAD_DPAD_UP));
    SDL_SetJoystickVirtualButton(js, SDL_GAMEPAD_BUTTON_SOUTH, false);
    SDL_Delay(30);
    xbox_InputHostPad(0, &buttons, 0, 0, 0, 0, 0, 0);
    CHECK(!(buttons & XINPUT_GAMEPAD_A) && (buttons & XINPUT_GAMEPAD_START));

    /* Sticks keep their sign on X and flip on Y (SDL's Y points down); -32768 stays in range. */
    SDL_SetJoystickVirtualAxis(js, SDL_GAMEPAD_AXIS_LEFTX, 20000);
    SDL_SetJoystickVirtualAxis(js, SDL_GAMEPAD_AXIS_LEFTY, -32768);
    SDL_SetJoystickVirtualAxis(js, SDL_GAMEPAD_AXIS_RIGHTX, -12000);
    SDL_SetJoystickVirtualAxis(js, SDL_GAMEPAD_AXIS_RIGHTY, 32767);
    SDL_Delay(30);
    xbox_InputHostPad(0, &buttons, &lt, &rt, &lx, &ly, &rx, &ry);
    CHECK(lx == 20000 && ly == 32767 && rx == -12000 && ry == -32768);

    /* Triggers scale 0..32767 to 0..255. A virtual trigger axis spans the full signed range, so
     * released is the minimum and the middle is half-pressed. */
    SDL_SetJoystickVirtualAxis(js, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 32767);
    SDL_SetJoystickVirtualAxis(js, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, -32768);
    SDL_Delay(30);
    xbox_InputHostPad(0, &buttons, &lt, &rt, &lx, &ly, &rx, &ry);
    CHECK(lt == 255 && rt == 0);

    /* The title's rumble reaches the device. */
    CHECK(xbox_InputSetState(0, &vib) == ERROR_SUCCESS);
    SDL_Delay(30);
    CHECK(rumble_low == 0x8000 && rumble_high == 0x4000);
    vib.wLeftMotorSpeed = vib.wRightMotorSpeed = 0;
    xbox_InputSetState(0, &vib);
    SDL_Delay(30);
    CHECK(rumble_low == 0 && rumble_high == 0);

    /* Unplug: the port reports no device; the title's calls fail rather than reading a dead pad. */
    SDL_CloseJoystick(js);
    SDL_DetachVirtualJoystick(id);
    CHECK(wait_present(0));
    CHECK(xbox_InputSetState(0, &vib) == ERROR_DEVICE_NOT_CONNECTED);

    /* Replug: found again by the hot-plug scan, with clean state. */
    js = attach(&id);
    CHECK(js != NULL);
    CHECK(wait_present(1));
    xbox_InputHostPad(0, &buttons, &lt, &rt, &lx, &ly, &rx, &ry);
    CHECK(buttons == 0 && lt == 0 && rt == 0);

    SDL_CloseJoystick(js);
    SDL_DetachVirtualJoystick(id);
    SDL_Quit();
    printf("SDL pad contract: %u checks, %u failures\n", checks, failures);
    return failures != 0;
}
