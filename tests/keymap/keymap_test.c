/* RECOMP_KEYMAP rebinds the keyboard pad; unnamed inputs keep their defaults.
 * RECOMP_MENU_KEYS adds menu navigation only while the probe reports a menu. */
#include "../../src/input/xinput_device.c"
#include <stdio.h>

static unsigned char held[256];
int xbox_FramebufferKeyDown(int vk) { return vk >= 0 && vk < 256 && held[vk]; }
static int menu_flag;
static int probe_menu(void) { return menu_flag; }
int xbox_FramebufferMouse(float *x, float *y) { *x = *y = 0; return 0; }
void nv2a_d3d_collect_menu_rects(int on) { (void)on; }

int main(void)
{
    XBOX_INPUT_STATE s;
    _putenv_s("RECOMP_KEYMAP", "A:57,RX-:2,LX-:41,START:0");
    _putenv_s("RECOMP_MENU_KEYS", "1"); /* read once, like the keymap */
    xbox_InputSetMenuProbe(probe_menu);
    held['W'] = held[VK_RBUTTON] = held['Z'] = held[VK_RETURN] = held['A'] = 1;
    keyboard_state(&s);
    if (s.Gamepad.bAnalogButtons[XBOX_BUTTON_A] != 255) { puts("FAIL: A not on W"); return 1; }
    if (s.Gamepad.sThumbRX != -32767) { puts("FAIL: RX- not on right mouse"); return 2; }
    if (s.Gamepad.sThumbLX != -32767) { puts("FAIL: LX- not on A"); return 3; }
    if (s.Gamepad.bAnalogButtons[XBOX_BUTTON_X] != 255) { puts("FAIL: X lost its default A"); return 4; }
    if (s.Gamepad.wButtons & XBOX_GAMEPAD_START) { puts("FAIL: START:0 still bound"); return 5; }
    held['W'] = 0;
    keyboard_state(&s);
    if (s.Gamepad.bAnalogButtons[XBOX_BUTTON_A]) { puts("FAIL: A still on Z"); return 6; }
    /* Menu keys: A is d-pad left (not X), a left click is A, only in menus. */
    memset(held, 0, sizeof held);
    held['A'] = held[VK_LBUTTON] = 1;
    menu_flag = 1;
    keyboard_state(&s);
    if (!(s.Gamepad.wButtons & XBOX_GAMEPAD_DPAD_LEFT)) { puts("FAIL: A is not left in a menu"); return 7; }
    if (s.Gamepad.sThumbLX) { puts("FAIL: A still moves the stick in a menu"); return 8; }
    if (s.Gamepad.bAnalogButtons[XBOX_BUTTON_A] != 255) { puts("FAIL: left click is not A in a menu"); return 9; }
    menu_flag = 0;
    keyboard_state(&s);
    if (s.Gamepad.wButtons & XBOX_GAMEPAD_DPAD_LEFT) { puts("FAIL: menu keys outside a menu"); return 10; }
    if (s.Gamepad.sThumbLX != -32767) { puts("FAIL: A lost its binding outside a menu"); return 11; }
    /* RECOMP_PADMAP: shoulders swapped, A on the right trigger, Start on A. */
    {
        XINPUT_GAMEPAD g = {0};
        XBOX_GAMEPAD out;
        _putenv_s("RECOMP_PADMAP", "BLACK:RB,WHITE:LB,A:RT,START:A");
        g.wButtons = XINPUT_GAMEPAD_RIGHT_SHOULDER | XINPUT_GAMEPAD_A | XINPUT_GAMEPAD_B;
        g.bRightTrigger = 200;
        pad_buttons(&g, &out);
        if (out.bAnalogButtons[XBOX_BUTTON_BLACK] != 255 || out.bAnalogButtons[XBOX_BUTTON_WHITE]) { puts("FAIL: shoulders not swapped"); return 12; }
        if (out.bAnalogButtons[XBOX_BUTTON_A] != 200) { puts("FAIL: A not on the right trigger"); return 13; }
        if (!(out.wButtons & XBOX_GAMEPAD_START)) { puts("FAIL: Start not on A"); return 14; }
        if (out.bAnalogButtons[XBOX_BUTTON_B] != 255) { puts("FAIL: B lost its own button"); return 15; }
    }
    puts("PASS: RECOMP_KEYMAP rebinds keys and mouse buttons; menu keys only in menus; RECOMP_PADMAP remaps the pad");
    return 0;
}
