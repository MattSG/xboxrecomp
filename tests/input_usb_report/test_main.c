/* Host gamepad to USB report contract, without a game or a physical controller.
 *
 * Build with the CMakeLists beside this file (cmake -S . -B build; cmake --build build; ctest).
 *
 * The host pad is fixed through the fixture below. The report encoder and its
 * trigger, button, stick and short-buffer rules are the production code. */
#include <windows.h>
#include <xinput.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static XINPUT_STATE host_state;
static BOOL host_connected;

/* The production pad merge reads its host pad through host_pad_state; here the test supplies it. */
#define XBOX_INPUT_HOST_PAD 1
static DWORD host_pad_state(DWORD port, XINPUT_STATE *state)
{
    if (!host_connected || port != 0) return ERROR_DEVICE_NOT_CONNECTED;
    *state = host_state;
    return ERROR_SUCCESS;
}

#include "input/xinput_device.c"
#include "usb/usb_gamepad.c"

/* The framebuffer window is outside this fixture: no keys, no mouse, no capture. */
int xbox_FramebufferHostInputCaptured(void) { return 0; }
int xbox_FramebufferMouse(float *x, float *y) { (void)x; (void)y; return 0; }
void xbox_FramebufferSetMousePressHandler(int (*handler)(float x, float y)) { (void)handler; }

/* Rendering is outside this fixture. */
ptrdiff_t xbox_GetMemoryOffset(void) { return 0; }
bool nv2a_hook_pvideo_snapshot(uint32_t regs[0x1000 / 4])
{
    (void)regs; return false;
}
void nv2a_hook_pvideo_consume(unsigned bank) { (void)bank; }
float nv2a_d3d_display_aspect(void) { return 4.0f / 3.0f; }

static unsigned checks, failures;
#define CHECK(condition) do { \
    ++checks; \
    if (!(condition)) { \
        fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); \
        ++failures; \
    } \
} while (0)

static void report(uint8_t out[20])
{
    CHECK(usb_gamepad_report(0, out, 20) == 20);
    CHECK(out[0] == 0 && out[1] == 20 && out[3] == 0);
}

static int16_t report_axis(const uint8_t out[20], int offset)
{
    return (int16_t)((uint16_t)out[offset] | ((uint16_t)out[offset + 1] << 8));
}

static void expect_triggers(unsigned left, unsigned right)
{
    uint8_t out[20];
    report(out);
    CHECK(out[10] == left);
    CHECK(out[11] == right);
}

static void check_host_report(void)
{
    static const uint8_t pressures[] = {0, 1, 29, 30, 31, 127, 128, 254, 255};
    uint8_t out[20], short_report[20];
    unsigned i, j;
    host_connected = TRUE;
    memset(&host_state, 0, sizeof(host_state));
    /* No deadzone, truncation or coupling may be introduced between triggers. */
    for (i = 0; i < sizeof pressures; ++i) {
        for (j = 0; j < sizeof pressures; ++j) {
            host_state.Gamepad.bLeftTrigger = pressures[i];
            host_state.Gamepad.bRightTrigger = pressures[j];
            expect_triggers(pressures[i], pressures[j]);
        }
    }
    memset(&host_state, 0, sizeof(host_state));
    host_state.Gamepad.wButtons = XINPUT_GAMEPAD_RIGHT_SHOULDER;
    report(out);
    CHECK(out[8] == 255 && out[9] == 0); /* RB is Xbox Black. */
    host_state.Gamepad.wButtons = XINPUT_GAMEPAD_LEFT_SHOULDER;
    report(out);
    CHECK(out[8] == 0 && out[9] == 255); /* LB is Xbox White. */
    host_state.Gamepad.wButtons = XINPUT_GAMEPAD_A | XINPUT_GAMEPAD_B |
        XINPUT_GAMEPAD_X | XINPUT_GAMEPAD_Y | XINPUT_GAMEPAD_START |
        XINPUT_GAMEPAD_DPAD_RIGHT | XINPUT_GAMEPAD_LEFT_THUMB;
    host_state.Gamepad.sThumbLX = -32768;
    host_state.Gamepad.sThumbLY = 32767;
    host_state.Gamepad.sThumbRX = -12345;
    host_state.Gamepad.sThumbRY = 12345;
    report(out);
    CHECK(out[2] == (XBOX_GAMEPAD_START | XBOX_GAMEPAD_DPAD_RIGHT |
                     XBOX_GAMEPAD_LEFT_THUMB));
    CHECK(out[4] == 255 && out[5] == 255 && out[6] == 255 && out[7] == 255);
    CHECK(report_axis(out, 12) == -32768 && report_axis(out, 14) == 32767);
    CHECK(report_axis(out, 16) == -12345 && report_axis(out, 18) == 12345);
    memset(short_report, 0xA5, sizeof short_report);
    CHECK(usb_gamepad_report(0, short_report, 19) == 0);
    for (i = 0; i < sizeof short_report; ++i) CHECK(short_report[i] == 0xA5);
    host_connected = FALSE;
}

int main(void)
{
    check_host_report();
    printf("host report input contract: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
