/*
 * input_host.c -- the host controller behind the replaced XAPI input.
 *
 * Adapted from doaxbv-re (https://github.com/NoRain211/doaxbv-re,
 * recomp-runtime/input_host_win32.c), GPL-3.0: XInput pad 0 and the keyboard,
 * mapped to the Xbox gamepad. Added here: RECOMP_FAKE_INPUT, the scripted
 * presses src/usb/usb_gamepad.c already understood, so an unattended run can
 * still get past "Press START"; and RECOMP_INPUT_SEQ, a one-shot timed
 * sequence that walks a menu path the same way every run.
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
    { "white", 1, HOST_ANALOG_WHITE }, { "black", 1, HOST_ANALOG_BLACK },
    { "lt",    1, HOST_ANALOG_LTRIG }, { "rt",    1, HOST_ANALOG_RTRIG },
};

/* Name -> FAKE index, or -1. `n` is the name's length inside a longer spec. */
static int fake_lookup(const char *p, size_t n)
{
    int i;
    for (i = 0; i < (int)(sizeof FAKE / sizeof FAKE[0]); i++)
        if (strlen(FAKE[i].name) == n && strncmp(p, FAKE[i].name, n) == 0)
            return i;
    return -1;
}

static void fake_apply(RecompInputGamepad *g, int i)
{
    if (FAKE[i].analog)
        g->analog_buttons[FAKE[i].value] = 0xFFu;
    else
        g->buttons |= (uint16_t)FAKE[i].value;
}

/* RECOMP_INPUT_SEQ=1500:start,4000:a,6000:down+a:400 -- a one-shot script.
 *
 * Each step is <ms>:<button>[+<button>...][:<hold ms>]. The clock starts the
 * first time the title reads the pad, so a slow boot does not eat the script;
 * hold defaults to 200 ms, long enough for a title polling at 60 Hz to see the
 * press and the release. Steps may overlap, which is how a chord is held.
 *
 * This exists because RECOMP_FAKE_INPUT cycles forever: it is right for
 * "get past Press START" and wrong for a menu, where it keeps pressing things
 * at whatever screen it reached. A menu path is a fixed sequence, and there is
 * no save state to restore a native process to -- lifted code is mid-flight
 * on real threads with host GPU objects behind it -- so replaying the presses
 * is how a run gets back to the same screen. The sequence is the save state.
 * Nothing is pressed after the last step ends. */
#define SEQ_MAX 128
static struct seq_step { unsigned long at, hold; int keys[4]; int nkeys; } s_seq[SEQ_MAX];
static int s_seq_count = -1;
static unsigned long long s_seq_t0;

static void seq_parse(const char *env)
{
    const char *p = env;

    while (*p && s_seq_count < SEQ_MAX) {
        struct seq_step *s = &s_seq[s_seq_count];
        char *end;
        const char *step_end = strchr(p, ',');
        size_t len = step_end ? (size_t)(step_end - p) : strlen(p);
        const char *q;

        memset(s, 0, sizeof *s);
        s->hold = 200;
        s->at = strtoul(p, &end, 10);
        if (end == p || *end != ':') {
            fprintf(stderr, "  [PAD] RECOMP_INPUT_SEQ: bad step at '%.*s' (want <ms>:<button>)\n",
                    (int)len, p);
            return;
        }
        q = end + 1;
        while (q < p + len) {
            size_t n = 0;
            int k;
            while (q + n < p + len && q[n] != '+' && q[n] != ':')
                n++;
            k = fake_lookup(q, n);
            if (k < 0) {
                fprintf(stderr, "  [PAD] RECOMP_INPUT_SEQ: unknown button '%.*s'\n", (int)n, q);
                return;
            }
            if (s->nkeys < 4)
                s->keys[s->nkeys++] = k;
            q += n;
            if (q < p + len && *q == ':') {
                s->hold = strtoul(q + 1, NULL, 10);
                if (!s->hold)
                    s->hold = 200;
                break;
            }
            if (q < p + len && *q == '+')
                q++;
        }
        s_seq_count++;
        if (!step_end)
            break;
        p = step_end + 1;
    }
}

static void seq_input(RecompInputGamepad *g)
{
    unsigned long long t;
    int i, k;

    if (s_seq_count < 0) {
        const char *env = getenv("RECOMP_INPUT_SEQ");
        s_seq_count = 0;
        if (env && *env) {
            seq_parse(env);
            fprintf(stderr, "  [PAD] input script: %d steps from %s\n", s_seq_count, env);
            fflush(stderr);
        }
    }
    if (!s_seq_count)
        return;
    if (!s_seq_t0)
        s_seq_t0 = GetTickCount64();
    t = GetTickCount64() - s_seq_t0;
    for (i = 0; i < s_seq_count; i++) {
        if (t < s_seq[i].at || t >= (unsigned long long)s_seq[i].at + s_seq[i].hold)
            continue;
        for (k = 0; k < s_seq[i].nkeys; k++)
            fake_apply(g, s_seq[i].keys[k]);
    }
}

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
    {
        const char *e = strchr(p, ',');
        i = fake_lookup(p, e ? (size_t)(e - p) : strlen(p));
        if (i >= 0)
            fake_apply(g, i);
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
    seq_input(gamepad);
    return true;
}
