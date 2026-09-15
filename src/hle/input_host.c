/*
 * input_host.c -- the host controller behind the replaced XAPI input.
 *
 * Adapted from doaxbv-re (https://github.com/NoRain211/doaxbv-re,
 * recomp-runtime/input_host_win32.c), GPL-3.0: XInput pad 0 and the keyboard,
 * mapped to the Xbox gamepad. Added here: RECOMP_FAKE_INPUT, the scripted
 * presses src/usb/usb_gamepad.c already understood, so an unattended run can
 * still get past "Press START".
 *
 * Keyboard: arrows = D-pad, Enter = START, Backspace = BACK, Z = A, X = B,
 * A = X, S = Y, Q = White, W = Black, E = left trigger, R = right trigger.
 */
#include "input_host.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <xinput.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    XBOX_DPAD_UP = 0x0001u,
    XBOX_DPAD_DOWN = 0x0002u,
    XBOX_DPAD_LEFT = 0x0004u,
    XBOX_DPAD_RIGHT = 0x0008u,
    XBOX_START = 0x0010u,
    XBOX_BACK = 0x0020u,
};

static bool pressed(int key)
{
    return (GetAsyncKeyState(key) & 0x8000) != 0;
}

/* RECOMP_FAKE_INPUT=start,a  presses each listed button in turn, one per
 * cycle; RECOMP_FAKE_INPUT_MS (default 500) is how long each press and each
 * gap lasts. Pressed and released rather than held: menus act on the edge. */
static const struct { const char *name; int analog; unsigned value; } FAKE[] = {
    { "start", 0, XBOX_START },      { "back",  0, XBOX_BACK },
    { "up",    0, XBOX_DPAD_UP },    { "down",  0, XBOX_DPAD_DOWN },
    { "left",  0, XBOX_DPAD_LEFT },  { "right", 0, XBOX_DPAD_RIGHT },
    { "a",     1, HOST_ANALOG_A },   { "b",     1, HOST_ANALOG_B },
    { "x",     1, HOST_ANALOG_X },   { "y",     1, HOST_ANALOG_Y },
};

static void fake_input(RecompInputGamepad *g)
{
    static int checked;
    static char spec[64];
    static unsigned long period;
    const char *p;
    unsigned long long now;
    int count = 1, want, index = 0, i;

    if (!checked) {
        const char *env = getenv("RECOMP_FAKE_INPUT");
        const char *ms = getenv("RECOMP_FAKE_INPUT_MS");
        checked = 1;
        if (env) {
            strncpy(spec, env, sizeof spec - 1);
            fprintf(stderr, "  [PAD] synthetic input: %s\n", spec);
            fflush(stderr);
        }
        period = ms ? strtoul(ms, NULL, 0) : 0;
        if (!period)
            period = 500;
    }
    if (!spec[0])
        return;
    now = GetTickCount64();
    if ((now / period) % 2 == 0)
        return;                                 /* the gap half of the cycle */
    for (p = spec; *p; p++)
        if (*p == ',')
            count++;
    want = (int)((now / (period * 2)) % (unsigned long long)count);
    p = spec;
    while (index < want && (p = strchr(p, ',')) != NULL) {
        p++;
        index++;
    }
    if (!p)
        return;
    for (i = 0; i < (int)(sizeof FAKE / sizeof FAKE[0]); i++) {
        size_t n = strlen(FAKE[i].name);
        if (strncmp(p, FAKE[i].name, n) != 0 || (p[n] && p[n] != ','))
            continue;
        if (FAKE[i].analog)
            g->analog_buttons[FAKE[i].value] = 0xFFu;
        else
            g->buttons |= (uint16_t)FAKE[i].value;
        return;
    }
}

bool recomp_input_host_sample(RecompInputGamepad *gamepad)
{
    XINPUT_STATE state;

    if (gamepad == NULL)
        return false;
    memset(gamepad, 0, sizeof *gamepad);
    memset(&state, 0, sizeof state);
    if (XInputGetState(0u, &state) == ERROR_SUCCESS) {
        WORD digital = XINPUT_GAMEPAD_DPAD_UP | XINPUT_GAMEPAD_DPAD_DOWN |
                       XINPUT_GAMEPAD_DPAD_LEFT | XINPUT_GAMEPAD_DPAD_RIGHT |
                       XINPUT_GAMEPAD_START | XINPUT_GAMEPAD_BACK |
                       XINPUT_GAMEPAD_LEFT_THUMB | XINPUT_GAMEPAD_RIGHT_THUMB;
        WORD b = state.Gamepad.wButtons;

        gamepad->buttons = b & digital;
        gamepad->analog_buttons[HOST_ANALOG_A] = (b & XINPUT_GAMEPAD_A) ? 0xFFu : 0u;
        gamepad->analog_buttons[HOST_ANALOG_B] = (b & XINPUT_GAMEPAD_B) ? 0xFFu : 0u;
        gamepad->analog_buttons[HOST_ANALOG_X] = (b & XINPUT_GAMEPAD_X) ? 0xFFu : 0u;
        gamepad->analog_buttons[HOST_ANALOG_Y] = (b & XINPUT_GAMEPAD_Y) ? 0xFFu : 0u;
        gamepad->analog_buttons[HOST_ANALOG_BLACK] =
            (b & XINPUT_GAMEPAD_LEFT_SHOULDER) ? 0xFFu : 0u;
        gamepad->analog_buttons[HOST_ANALOG_WHITE] =
            (b & XINPUT_GAMEPAD_RIGHT_SHOULDER) ? 0xFFu : 0u;
        gamepad->analog_buttons[HOST_ANALOG_LTRIG] = state.Gamepad.bLeftTrigger;
        gamepad->analog_buttons[HOST_ANALOG_RTRIG] = state.Gamepad.bRightTrigger;
        gamepad->thumb_lx = state.Gamepad.sThumbLX;
        gamepad->thumb_ly = state.Gamepad.sThumbLY;
        gamepad->thumb_rx = state.Gamepad.sThumbRX;
        gamepad->thumb_ry = state.Gamepad.sThumbRY;
    }
    if (pressed(VK_UP))     gamepad->buttons |= XBOX_DPAD_UP;
    if (pressed(VK_DOWN))   gamepad->buttons |= XBOX_DPAD_DOWN;
    if (pressed(VK_LEFT))   gamepad->buttons |= XBOX_DPAD_LEFT;
    if (pressed(VK_RIGHT))  gamepad->buttons |= XBOX_DPAD_RIGHT;
    if (pressed(VK_RETURN)) gamepad->buttons |= XBOX_START;
    if (pressed(VK_BACK))   gamepad->buttons |= XBOX_BACK;
    if (pressed('Z')) gamepad->analog_buttons[HOST_ANALOG_A] = 0xFFu;
    if (pressed('X')) gamepad->analog_buttons[HOST_ANALOG_B] = 0xFFu;
    if (pressed('A')) gamepad->analog_buttons[HOST_ANALOG_X] = 0xFFu;
    if (pressed('S')) gamepad->analog_buttons[HOST_ANALOG_Y] = 0xFFu;
    if (pressed('Q')) gamepad->analog_buttons[HOST_ANALOG_WHITE] = 0xFFu;
    if (pressed('W')) gamepad->analog_buttons[HOST_ANALOG_BLACK] = 0xFFu;
    if (pressed('E')) gamepad->analog_buttons[HOST_ANALOG_LTRIG] = 0xFFu;
    if (pressed('R')) gamepad->analog_buttons[HOST_ANALOG_RTRIG] = 0xFFu;
    fake_input(gamepad);
    return true;
}
