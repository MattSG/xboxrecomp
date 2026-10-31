/* RECOMP_KEYMAP rebinds the keyboard pad; unnamed inputs keep their defaults.
 * RECOMP_MENU_KEYS adds menu navigation only while the probe reports a menu.
 * A title's keyboard hook sees the pad last, with the menu state. */
#include "../../src/input/xinput_xbox.h"
static ULONGLONG test_ticks = 1000;
static ULONGLONG click_clock(void) { return test_ticks; }
#define GetTickCount64 click_clock
#include "../../src/input/xinput_device.c"
#undef GetTickCount64
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#ifdef _WIN32
#define set_env(name, value) _putenv_s(name, value)
#else
#define set_env(name, value) setenv(name, value, 1)
#endif

/* The window's key table (keyboard.c), written as the window would. */
static void press(int vk, int down) { xbox_InputKeySet(vk, down); }
int xbox_FramebufferHostInputCaptured(void) { return 0; }
void xbox_FramebufferSetMousePressHandler(int (*handler)(float, float)) { (void)handler; }
static int menu_flag, hook_calls;
static BOOL hook_menu;
static int probe_menu(void) { return menu_flag; }
static void hook(XBOX_INPUT_STATE *s, BOOL menu)
{
    ++hook_calls;
    hook_menu = menu;
    if (xbox_FramebufferKeyDown('R')) s->Gamepad.bAnalogButtons[XBOX_BUTTON_B] = 255;
}
int xbox_FramebufferMouse(float *x, float *y) { *x = *y = 0; return 0; }

static int consume_click(float x, float y) { assert(x == 0 && y == 0); return 1; }

int main(int argc, char **argv)
{
    XBOX_INPUT_STATE s;
    if (argc == 2 && !strcmp(argv[1], "menu-click")) {
        assert(!menu_click());
        xbox_InputSetMenuClickHandler(consume_click);
        press(VK_LBUTTON, 1);
        assert(!menu_click()); /* Consumed native click never becomes pad A. */
        assert(!menu_click());
        press(VK_LBUTTON, 0); assert(!menu_click());
        xbox_InputSetMenuClickHandler(NULL);
        press(VK_LBUTTON, 1); assert(menu_click());
        test_ticks += 79; assert(menu_click());
        test_ticks++; assert(!menu_click()); /* Held button does not retrigger. */
        press(VK_LBUTTON, 0); assert(!menu_click());
        press(VK_LBUTTON, 1); assert(menu_click());
        puts("PASS: consumed clicks and bounded mouse-to-A fallback");
        return 0;
    }

#ifndef _WIN32
    if (argc == 2 && !strcmp(argv[1], "two-gamepads")) {
        SDL_VirtualJoystickDesc desc;
        SDL_JoystickID ids[2];
        XINPUT_STATE state;
        if (!SDL_Init(SDL_INIT_GAMEPAD)) return 30;
        SDL_INIT_INTERFACE(&desc);
        desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
        desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
        desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
        desc.axis_mask = (1u << SDL_GAMEPAD_AXIS_COUNT) - 1u;
        desc.button_mask = (1u << SDL_GAMEPAD_BUTTON_COUNT) - 1u;
        desc.name = "discovery regression";
        ids[0] = SDL_AttachVirtualJoystick(&desc);
        ids[1] = SDL_AttachVirtualJoystick(&desc);
        SDL_UpdateGamepads();
        int ok = ids[0] && ids[1] &&
            host_pad_state(0, &state) == ERROR_SUCCESS &&
            host_pad_state(1, &state) == ERROR_SUCCESS;
        for (unsigned i = 0; i < XBOX_MAX_CONTROLLERS; ++i)
            if (g_sdl_pads[i]) SDL_CloseGamepad(g_sdl_pads[i]);
        if (ids[0]) SDL_DetachVirtualJoystick(ids[0]);
        if (ids[1]) SDL_DetachVirtualJoystick(ids[1]);
        SDL_Quit();
        if (!ok) return 31;
        puts("PASS: two gamepads discovered on consecutive port polls");
        return 0;
    }
#endif
    if (argc == 2 && !strcmp(argv[1], "default-padmap")) {
        XINPUT_GAMEPAD g = {0};
        XBOX_GAMEPAD out;
        set_env("RECOMP_PADMAP", "");
        g.wButtons = XINPUT_GAMEPAD_RIGHT_SHOULDER;
        pad_buttons(&g, &out);
        if (out.bAnalogButtons[XBOX_BUTTON_BLACK] != 255 || out.bAnalogButtons[XBOX_BUTTON_WHITE]) return 21;
        g.wButtons = XINPUT_GAMEPAD_LEFT_SHOULDER;
        pad_buttons(&g, &out);
        if (out.bAnalogButtons[XBOX_BUTTON_WHITE] != 255 || out.bAnalogButtons[XBOX_BUTTON_BLACK]) return 22;
        puts("PASS: right shoulder is Black and left shoulder is White by default");
        return 0;
    }
    set_env("RECOMP_KEYMAP", "A:57,RX-:2,LX-:41,START:0");
    set_env("RECOMP_MENU_KEYS", "1"); /* read once, like the keymap */
    set_env("RECOMP_KEYBOARD", "1");
    xbox_InputSetMenuProbe(probe_menu);
    press('W', 1); press(VK_RBUTTON, 1); press('Z', 1); press(VK_RETURN, 1); press('A', 1); press('C', 1);
    keyboard_pad(&s);
    if (s.Gamepad.bAnalogButtons[XBOX_BUTTON_A] != 255) { puts("FAIL: A not on W"); return 1; }
    if (s.Gamepad.sThumbRX != -32767) { puts("FAIL: RX- not on right mouse"); return 2; }
    if (s.Gamepad.sThumbLX != -32767) { puts("FAIL: LX- not on A"); return 3; }
    if (s.Gamepad.bAnalogButtons[XBOX_BUTTON_X] != 255) { puts("FAIL: X lost its default C"); return 4; }
    if (s.Gamepad.wButtons & XBOX_GAMEPAD_START) { puts("FAIL: START:0 still bound"); return 5; }
    press('W', 0);
    keyboard_pad(&s);
    if (s.Gamepad.bAnalogButtons[XBOX_BUTTON_A]) { puts("FAIL: A still on Z"); return 6; }
    /* Menu keys: A is d-pad left (not X), a left click is A, only in menus. */
    xbox_InputKeysClear();
    press('A', 1); press(VK_LBUTTON, 1);
    menu_flag = 1;
    keyboard_pad(&s);
    if (!(s.Gamepad.wButtons & XBOX_GAMEPAD_DPAD_LEFT)) { puts("FAIL: A is not left in a menu"); return 7; }
    if (s.Gamepad.sThumbLX) { puts("FAIL: A still moves the stick in a menu"); return 8; }
    if (s.Gamepad.bAnalogButtons[XBOX_BUTTON_A] != 255) { puts("FAIL: left click is not A in a menu"); return 9; }
    menu_flag = 0;
    keyboard_pad(&s);
    if (s.Gamepad.wButtons & XBOX_GAMEPAD_DPAD_LEFT) { puts("FAIL: menu keys outside a menu"); return 10; }
    if (s.Gamepad.sThumbLX != -32767) { puts("FAIL: A lost its binding outside a menu"); return 11; }
    /* RECOMP_PADMAP: shoulders swapped, A on the right trigger, Start on A. */
    {
        XINPUT_GAMEPAD g = {0};
        XBOX_GAMEPAD out;
        set_env("RECOMP_PADMAP", "BLACK:LB,WHITE:RB,A:RT,START:A");
        g.wButtons = XINPUT_GAMEPAD_LEFT_SHOULDER | XINPUT_GAMEPAD_A | XINPUT_GAMEPAD_B;
        g.bRightTrigger = 200;
        pad_buttons(&g, &out);
        if (out.bAnalogButtons[XBOX_BUTTON_BLACK] != 255 || out.bAnalogButtons[XBOX_BUTTON_WHITE]) { puts("FAIL: shoulders not swapped"); return 12; }
        if (out.bAnalogButtons[XBOX_BUTTON_A] != 200) { puts("FAIL: A not on the right trigger"); return 13; }
        if (!(out.wButtons & XBOX_GAMEPAD_START)) { puts("FAIL: Start not on A"); return 14; }
        if (out.bAnalogButtons[XBOX_BUTTON_B] != 255) { puts("FAIL: B lost its own button"); return 15; }
    }
    /* The title's hook runs on every read and sees the menu state. */
    xbox_InputKeysClear();
    menu_flag = 0;
    keyboard_pad(&s);
    if (hook_calls) { puts("FAIL: hook called before it was set"); return 16; }
    xbox_InputSetKeyboardHook(hook);
    press('R', 1);
    keyboard_pad(&s);
    if (hook_calls != 1 || hook_menu || s.Gamepad.bAnalogButtons[XBOX_BUTTON_B] != 255) { puts("FAIL: hook outside a menu"); return 17; }
    menu_flag = 1;
    keyboard_pad(&s);
    if (hook_calls != 2 || !hook_menu) { puts("FAIL: hook not told about the menu"); return 18; }
    xbox_InputSetKeyboardHook(NULL);
    keyboard_pad(&s);
    if (hook_calls != 2 || s.Gamepad.bAnalogButtons[XBOX_BUTTON_B]) { puts("FAIL: hook still set"); return 19; }
    puts("PASS: RECOMP_KEYMAP rebinds keys and mouse buttons; menu keys only in menus; RECOMP_PADMAP remaps the pad; the keyboard hook runs last");
    return 0;
}
