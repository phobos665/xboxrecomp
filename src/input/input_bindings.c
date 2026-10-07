/*
 * input_bindings.c -- read the binding config, and sample the four ports.
 *
 * Structure:
 *   CONTROLS[]   the 24 Xbox inputs, by the names the config file uses
 *   DEFAULTS[]   what each control reads with no config file: one pad source
 *                and, on port 1, one keyboard source
 *   a small JSON reader (no dependency is worth one file's worth of braces)
 *   resolve      binding strings -> Source records, once, at load
 *   sample       Source records -> an XBOX_GAMEPAD, every poll
 *
 * Sources are resolved to key codes and pad indices at load time, so a poll
 * does no string work. A poll reads each distinct key once (KeyCache) and
 * each pad once, because a title polls its pads several times a frame.
 *
 * Pads are read through recomp_pad.h: SDL3 by default, XInput on Windows
 * when the file's "pad_api" or RECOMP_PAD_API says so. The keyboard is
 * read with GetAsyncKeyState on Windows and not yet anywhere else. The
 * mouse -- its buttons as sources, and its movement as one controller's
 * stick -- is read from the game window through recomp_mouse.h.
 */
#include "input_bindings.h"
#include "recomp_pad.h"
#include "recomp_mouse.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#else
#  include <time.h>
#endif

#define MAX_SOURCES 4
#define PORTS XBOX_MAX_CONTROLLERS

/* XInput's own left-stick default. A stick is bound as two halves, so the
 * deadzone is applied per half rather than radially: a title that wants a
 * circular one applies its own on top, as it did on the console. */
#define DEADZONE_DEFAULT 7849

/* ---- the controls a title can read ------------------------------------- */

enum { K_DIGITAL, K_ANALOG, K_AXIS };

/* XINPUT_GAMEPAD_TRIGGER_THRESHOLD: the value XInput documents as the point
 * a trigger counts as pressed. */
#define TRIGGER_THRESHOLD 30
enum { AXIS_LX, AXIS_LY, AXIS_RX, AXIS_RY };

static const struct {
    const char *name;
    unsigned char kind;
    unsigned short arg;        /* digital: button bit; analog: index; axis: id */
    signed char sign;          /* axis only: which half of the axis */
} CONTROLS[] = {
    { "a",            K_ANALOG,  XBOX_BUTTON_A,            0 },
    { "b",            K_ANALOG,  XBOX_BUTTON_B,            0 },
    { "x",            K_ANALOG,  XBOX_BUTTON_X,            0 },
    { "y",            K_ANALOG,  XBOX_BUTTON_Y,            0 },
    { "black",        K_ANALOG,  XBOX_BUTTON_BLACK,        0 },
    { "white",        K_ANALOG,  XBOX_BUTTON_WHITE,        0 },
    { "start",        K_DIGITAL, XBOX_GAMEPAD_START,       0 },
    { "back",         K_DIGITAL, XBOX_GAMEPAD_BACK,        0 },
    { "ltrigger",     K_ANALOG,  XBOX_BUTTON_LTRIGGER,     0 },
    { "rtrigger",     K_ANALOG,  XBOX_BUTTON_RTRIGGER,     0 },
    { "dpad_up",      K_DIGITAL, XBOX_GAMEPAD_DPAD_UP,     0 },
    { "dpad_down",    K_DIGITAL, XBOX_GAMEPAD_DPAD_DOWN,   0 },
    { "dpad_left",    K_DIGITAL, XBOX_GAMEPAD_DPAD_LEFT,   0 },
    { "dpad_right",   K_DIGITAL, XBOX_GAMEPAD_DPAD_RIGHT,  0 },
    { "lstick_up",    K_AXIS,    AXIS_LY,                  1 },
    { "lstick_down",  K_AXIS,    AXIS_LY,                 -1 },
    { "lstick_left",  K_AXIS,    AXIS_LX,                 -1 },
    { "lstick_right", K_AXIS,    AXIS_LX,                  1 },
    { "lthumb",       K_DIGITAL, XBOX_GAMEPAD_LEFT_THUMB,  0 },
    { "rstick_up",    K_AXIS,    AXIS_RY,                  1 },
    { "rstick_down",  K_AXIS,    AXIS_RY,                 -1 },
    { "rstick_left",  K_AXIS,    AXIS_RX,                 -1 },
    { "rstick_right", K_AXIS,    AXIS_RX,                  1 },
    { "rthumb",       K_DIGITAL, XBOX_GAMEPAD_RIGHT_THUMB, 0 },
};
#define CONTROL_COUNT ((int)(sizeof CONTROLS / sizeof CONTROLS[0]))

/* The built-in mapping, and the one the UI offers as "reset to defaults".
 * The keyboard column is what this runtime has always used: arrows = D-pad,
 * Enter = START, Backspace = BACK, Z = A, X = B, A = X, S = Y, Q = White,
 * W = Black, E/R = triggers. It is applied to port 1 only. Sticks have no
 * keyboard default: every comfortable key is already a button here.
 *
 * tools/input_ui/test_runtime_vocabulary.py reads this table out of this file
 * and fails if the UI's defaults have drifted from it. */
static const struct { const char *control, *pad, *key; } DEFAULTS[] = {
    { "a",            "pad:a",          "key:Z"      },
    { "b",            "pad:b",          "key:X"      },
    { "x",            "pad:x",          "key:A"      },
    { "y",            "pad:y",          "key:S"      },
    { "black",        "pad:lshoulder",  "key:W"      },
    { "white",        "pad:rshoulder",  "key:Q"      },
    { "ltrigger",     "pad:lt",         "key:E"      },
    { "rtrigger",     "pad:rt",         "key:R"      },
    { "start",        "pad:start",      "key:RETURN" },
    { "back",         "pad:back",       "key:BACK"   },
    { "dpad_up",      "pad:dpad_up",    "key:UP"     },
    { "dpad_down",    "pad:dpad_down",  "key:DOWN"   },
    { "dpad_left",    "pad:dpad_left",  "key:LEFT"   },
    { "dpad_right",   "pad:dpad_right", "key:RIGHT"  },
    { "lstick_up",    "pad:ly+",        NULL         },
    { "lstick_down",  "pad:ly-",        NULL         },
    { "lstick_left",  "pad:lx-",        NULL         },
    { "lstick_right", "pad:lx+",        NULL         },
    { "lthumb",       "pad:lthumb",     NULL         },
    { "rstick_up",    "pad:ry+",        NULL         },
    { "rstick_down",  "pad:ry-",        NULL         },
    { "rstick_left",  "pad:rx-",        NULL         },
    { "rstick_right", "pad:rx+",        NULL         },
    { "rthumb",       "pad:rthumb",     NULL         },
};

/* ---- the sources a control can be bound to ----------------------------- */

enum { SRC_NONE, SRC_KEY, SRC_PAD_BUTTON, SRC_PAD_TRIGGER, SRC_PAD_AXIS, SRC_MOUSE };

/* Pad buttons, in the order PAD_BUTTONS names them. */
enum {
    PB_A, PB_B, PB_X, PB_Y, PB_LSHOULDER, PB_RSHOULDER, PB_START, PB_BACK,
    PB_LTHUMB, PB_RTHUMB, PB_DPAD_UP, PB_DPAD_DOWN, PB_DPAD_LEFT, PB_DPAD_RIGHT,
    PB_COUNT
};

static const char *const PAD_BUTTONS[PB_COUNT] = {
    "a", "b", "x", "y", "lshoulder", "rshoulder", "start", "back",
    "lthumb", "rthumb", "dpad_up", "dpad_down", "dpad_left", "dpad_right"
};

/* What a "mouse:" source may name, in RECOMP_MOUSE_LEFT.. order. A wheel
 * notch is a short press (recomp_mouse_source), so the wheel binds to a
 * button the way it does in a PC game: next weapon, previous weapon. */
static const char *const MOUSE_SOURCES[RECOMP_MOUSE_SOURCE_COUNT] = {
    "left", "right", "middle", "x1", "x2", "wheel_up", "wheel_down"
};

typedef struct {
    unsigned char kind;
    short arg;              /* key: virtual-key code; pad: button/axis index */
    signed char sign;       /* axis only */
} Source;

typedef struct {
    unsigned char count;
    Source src[MAX_SOURCES];
} Binding;

enum { DEV_NONE, DEV_KEYBOARD, DEV_PAD };

typedef struct {
    int device;
    int pad;                /* recomp_pad slot when device == DEV_PAD */
    int deadzone;
    int has_key;            /* any key: source, so the port works with no pad */
    int has_mouse;          /* any mouse: source, or the mouse moves its stick */
    Binding bind[CONTROL_COUNT];
    char label[40];
} Controller;

static Controller g_ctl[PORTS];
static char g_path[520];
static int g_have_path;

/* Mouse look and its settings: the file's top-level "mouse", then the
 * RECOMP_MOUSE_* variables over it. Handed to recomp_mouse.c at load. */
static RecompMouseConfig g_mouse;

/* Default lift for a moving mouse, as a fraction of the stick's travel:
 * just under the quarter that most titles treat as their own deadzone, so a
 * slow movement is not swallowed by it. */
#define MOUSE_ANTI_DEADZONE_DEFAULT 0.2

/* ---- names -> codes ---------------------------------------------------- */

/* Virtual-key names the config file may use. Single letters and digits are
 * their own code on Windows and are handled without a table entry; "key:#13"
 * passes a raw decimal code through for anything named nowhere. */
static const struct { const char *name; int vk; } KEYS[] = {
    { "UP", 0x26 }, { "DOWN", 0x28 }, { "LEFT", 0x25 }, { "RIGHT", 0x27 },
    { "RETURN", 0x0D }, { "ENTER", 0x0D }, { "BACK", 0x08 },
    { "BACKSPACE", 0x08 }, { "SPACE", 0x20 }, { "TAB", 0x09 },
    { "ESCAPE", 0x1B }, { "SHIFT", 0x10 }, { "LSHIFT", 0xA0 },
    { "RSHIFT", 0xA1 }, { "CTRL", 0x11 }, { "LCTRL", 0xA2 }, { "RCTRL", 0xA3 },
    { "ALT", 0x12 }, { "LALT", 0xA4 }, { "RALT", 0xA5 },
    { "CAPSLOCK", 0x14 }, { "INSERT", 0x2D }, { "DELETE", 0x2E },
    { "HOME", 0x24 }, { "END", 0x23 }, { "PAGEUP", 0x21 }, { "PAGEDOWN", 0x22 },
    { "F1", 0x70 }, { "F2", 0x71 }, { "F3", 0x72 }, { "F4", 0x73 },
    { "F5", 0x74 }, { "F6", 0x75 }, { "F7", 0x76 }, { "F8", 0x77 },
    { "F9", 0x78 }, { "F10", 0x79 }, { "F11", 0x7A }, { "F12", 0x7B },
    { "NUMPAD0", 0x60 }, { "NUMPAD1", 0x61 }, { "NUMPAD2", 0x62 },
    { "NUMPAD3", 0x63 }, { "NUMPAD4", 0x64 }, { "NUMPAD5", 0x65 },
    { "NUMPAD6", 0x66 }, { "NUMPAD7", 0x67 }, { "NUMPAD8", 0x68 },
    { "NUMPAD9", 0x69 }, { "MULTIPLY", 0x6A }, { "ADD", 0x6B },
    { "SUBTRACT", 0x6D }, { "DECIMAL", 0x6E }, { "DIVIDE", 0x6F },
    { "SEMICOLON", 0xBA }, { "EQUALS", 0xBB }, { "COMMA", 0xBC },
    { "MINUS", 0xBD }, { "PERIOD", 0xBE }, { "SLASH", 0xBF },
    { "GRAVE", 0xC0 }, { "LBRACKET", 0xDB }, { "BACKSLASH", 0xDC },
    { "RBRACKET", 0xDD }, { "QUOTE", 0xDE },
};

static int control_index(const char *name)
{
    int i;
    for (i = 0; i < CONTROL_COUNT; i++)
        if (strcmp(CONTROLS[i].name, name) == 0)
            return i;
    return -1;
}

static int key_code(const char *name)
{
    size_t i;

    if (name[0] == '#')
        return (int)strtol(name + 1, NULL, 10);
    if (name[1] == '\0') {
        char c = name[0];
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))
            return (int)(unsigned char)c;
        return 0;
    }
    for (i = 0; i < sizeof KEYS / sizeof KEYS[0]; i++)
        if (strcmp(KEYS[i].name, name) == 0)
            return KEYS[i].vk;
    return 0;
}

/* "pad:lx-", "key:RETURN", "none" -> a Source. Returns 0 on a name this
 * build does not know, which is reported once rather than per control. */
static int parse_source(const char *text, Source *out)
{
    memset(out, 0, sizeof *out);
    if (!text || !*text || strcmp(text, "none") == 0)
        return 1;                                     /* deliberately unbound */
    if (strncmp(text, "key:", 4) == 0) {
        int vk = key_code(text + 4);
        if (!vk)
            return 0;
        out->kind = SRC_KEY;
        out->arg = (short)vk;
        return 1;
    }
    if (strncmp(text, "pad:", 4) == 0) {
        const char *name = text + 4;
        int i;

        if (strcmp(name, "lt") == 0 || strcmp(name, "rt") == 0) {
            out->kind = SRC_PAD_TRIGGER;
            out->arg = (short)(name[0] == 'r');
            return 1;
        }
        for (i = 0; i < PB_COUNT; i++)
            if (strcmp(PAD_BUTTONS[i], name) == 0) {
                out->kind = SRC_PAD_BUTTON;
                out->arg = (short)i;
                return 1;
            }
        /* half axes: lx+ ly- rx+ ry- */
        if (strlen(name) == 3 && (name[2] == '+' || name[2] == '-') &&
            (name[0] == 'l' || name[0] == 'r') &&
            (name[1] == 'x' || name[1] == 'y')) {
            out->kind = SRC_PAD_AXIS;
            out->arg = (short)((name[0] == 'r' ? 2 : 0) + (name[1] == 'y'));
            out->sign = (signed char)(name[2] == '+' ? 1 : -1);
            return 1;
        }
    }
    if (strncmp(text, "mouse:", 6) == 0) {
        int i;

        for (i = 0; i < RECOMP_MOUSE_SOURCE_COUNT; i++)
            if (strcmp(MOUSE_SOURCES[i], text + 6) == 0) {
                out->kind = SRC_MOUSE;
                out->arg = (short)i;
                return 1;
            }
    }
    return 0;
}

static void bind_add(Binding *b, const char *text, int *bad)
{
    Source s;

    if (b->count >= MAX_SOURCES)
        return;
    if (!parse_source(text, &s)) {
        (*bad)++;
        return;
    }
    if (s.kind == SRC_NONE)
        return;
    b->src[b->count++] = s;
}

/* ---- defaults ---------------------------------------------------------- */

static void defaults_for(Controller *c, int port)
{
    int i, bad = 0;

    memset(c, 0, sizeof *c);
    c->device = DEV_PAD;
    c->pad = port;
    c->deadzone = DEADZONE_DEFAULT;
    for (i = 0; i < (int)(sizeof DEFAULTS / sizeof DEFAULTS[0]); i++) {
        int k = control_index(DEFAULTS[i].control);
        if (k < 0)
            continue;
        bind_add(&c->bind[k], DEFAULTS[i].pad, &bad);
        if (port == 0 && DEFAULTS[i].key)
            bind_add(&c->bind[k], DEFAULTS[i].key, &bad);
    }
    c->has_key = (port == 0);
}

/* ---- a small JSON reader ----------------------------------------------- */

static const char *ws(const char *p)
{
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
        p++;
    return p;
}

/* p is at the opening quote. Copies at most n-1 characters; the escapes a
 * config file can contain are handled and \u is left as written. */
static const char *jstring(const char *p, char *out, size_t n)
{
    size_t i = 0;

    if (*p != '"')
        return NULL;
    p++;
    while (*p && *p != '"') {
        char c = *p++;
        if (c == '\\' && *p) {
            char e = *p++;
            switch (e) {
            case 'n': c = '\n'; break;
            case 't': c = '\t'; break;
            case 'r': c = '\r'; break;
            default:  c = e;    break;
            }
        }
        if (i + 1 < n)
            out[i++] = c;
    }
    if (*p != '"')
        return NULL;
    out[i] = '\0';
    return p + 1;
}

static const char *jskip(const char *p)
{
    p = ws(p);
    if (*p == '"') {
        char discard[8];
        const char *q = p;
        /* jstring with a tiny buffer still walks the whole string. */
        return jstring(q, discard, sizeof discard);
    }
    if (*p == '{' || *p == '[') {
        char open = *p, close = (char)(open == '{' ? '}' : ']');
        int depth = 0;
        while (*p) {
            if (*p == '"') {
                char discard[8];
                const char *q = jstring(p, discard, sizeof discard);
                if (!q)
                    return NULL;
                p = q;
                continue;
            }
            if (*p == open)
                depth++;
            else if (*p == close && --depth == 0)
                return p + 1;
            p++;
        }
        return NULL;
    }
    while (*p && *p != ',' && *p != '}' && *p != ']')
        p++;
    return p;
}

/* { "a": "pad:a", "b": ["pad:b", "key:X"] } */
static const char *parse_bindings(const char *p, Controller *c, int *bad)
{
    char key[48], value[64];

    p = ws(p);
    if (*p != '{')
        return NULL;
    p = ws(p + 1);
    if (*p == '}')
        return p + 1;
    for (;;) {
        int k;

        p = jstring(p, key, sizeof key);
        if (!p)
            return NULL;
        p = ws(p);
        if (*p != ':')
            return NULL;
        p = ws(p + 1);
        k = control_index(key);
        if (k >= 0)
            c->bind[k].count = 0;      /* a named control replaces its default */
        if (*p == '[') {
            p = ws(p + 1);
            while (*p && *p != ']') {
                p = jstring(p, value, sizeof value);
                if (!p)
                    return NULL;
                if (k >= 0)
                    bind_add(&c->bind[k], value, bad);
                p = ws(p);
                if (*p == ',')
                    p = ws(p + 1);
            }
            if (*p != ']')
                return NULL;
            p++;
        } else if (*p == '"') {
            p = jstring(p, value, sizeof value);
            if (!p)
                return NULL;
            if (k >= 0)
                bind_add(&c->bind[k], value, bad);
        } else {
            p = jskip(p);                        /* null, or something odd */
            if (!p)
                return NULL;
        }
        if (k < 0)
            (*bad)++;
        p = ws(p);
        if (*p == ',') {
            p = ws(p + 1);
            continue;
        }
        if (*p != '}')
            return NULL;
        return p + 1;
    }
}

static void set_device(Controller *c, const char *name)
{
    if (strcmp(name, "keyboard") == 0) {
        c->device = DEV_KEYBOARD;
        c->pad = -1;
    } else if (strcmp(name, "none") == 0 || !*name) {
        c->device = DEV_NONE;
        c->pad = -1;
    } else if (strncmp(name, "gamepad:", 8) == 0 || strncmp(name, "xinput:", 7) == 0) {
        /* "gamepad:N" is the N-th pad through whichever API reads pads;
         * "xinput:N" is the name files written before SDL3 used, and means
         * the same slot. */
        int n = (int)strtol(strchr(name, ':') + 1, NULL, 10);
        c->device = DEV_PAD;
        c->pad = (n >= 0 && n < PORTS) ? n : 0;
    }
}

/* One element of "controllers". Starts from the defaults for whichever port
 * it names, so a file that lists two bindings is a two-binding change. */
static const char *parse_controller(const char *p, int index, int *bad)
{
    Controller tmp;
    char key[48], value[64];
    int port = index, seen_port = 0, i;

    defaults_for(&tmp, index < PORTS ? index : 0);
    p = ws(p);
    if (*p != '{')
        return NULL;
    p = ws(p + 1);
    while (*p && *p != '}') {
        p = jstring(p, key, sizeof key);
        if (!p)
            return NULL;
        p = ws(p);
        if (*p != ':')
            return NULL;
        p = ws(p + 1);
        if (strcmp(key, "port") == 0) {
            port = (int)strtol(p, NULL, 10) - 1;       /* the file is 1-based */
            seen_port = 1;
            p = jskip(p);
        } else if (strcmp(key, "device") == 0) {
            p = jstring(p, value, sizeof value);
            if (!p)
                return NULL;
            set_device(&tmp, value);
        } else if (strcmp(key, "deadzone") == 0) {
            tmp.deadzone = (int)strtol(p, NULL, 10);
            p = jskip(p);
        } else if (strcmp(key, "bindings") == 0) {
            p = parse_bindings(p, &tmp, bad);
            if (!p)
                return NULL;
        } else {
            p = jskip(p);
            if (!p)
                return NULL;
        }
        p = ws(p);
        if (*p == ',')
            p = ws(p + 1);
    }
    if (*p != '}')
        return NULL;
    if (port < 0 || port >= PORTS)
        return p + 1;                             /* a port nothing can read */
    /* "port" may come after "bindings", so the defaults this started from
     * could be the wrong port's. Only the pad index differs, and only when
     * the file did not say. */
    if (seen_port && tmp.device == DEV_PAD && tmp.pad == index && index != port)
        tmp.pad = port;
    tmp.has_key = 0;
    for (i = 0; i < CONTROL_COUNT; i++) {
        int s;
        for (s = 0; s < tmp.bind[i].count; s++) {
            if (tmp.bind[i].src[s].kind == SRC_KEY)
                tmp.has_key = 1;
            if (tmp.bind[i].src[s].kind == SRC_MOUSE)
                tmp.has_mouse = 1;
        }
    }
    g_ctl[port] = tmp;
    return p + 1;
}

/* "true", "false", or a number: what a JSON switch may be written as. */
static int jbool(const char *p)
{
    if (strncmp(p, "true", 4) == 0 || strncmp(p, "on", 2) == 0)
        return 1;
    if (strncmp(p, "false", 5) == 0 || strncmp(p, "null", 4) == 0 ||
        strncmp(p, "off", 3) == 0)
        return 0;
    return strtol(p, NULL, 10) != 0;
}

static int stick_by_name(const char *name, int fallback)
{
    if (strcmp(name, "right") == 0)
        return RECOMP_MOUSE_STICK_RIGHT;
    if (strcmp(name, "left") == 0)
        return RECOMP_MOUSE_STICK_LEFT;
    if (strcmp(name, "off") == 0 || strcmp(name, "none") == 0)
        return RECOMP_MOUSE_STICK_OFF;
    return fallback;
}

/* "mouse": { "stick": "right", "port": 1, "sensitivity": 1.0,
 *            "invert_y": false, "anti_deadzone": 0.2 }
 * Top level rather than per controller because there is one mouse: it can
 * move one controller's stick, and "port" says which. */
static const char *parse_mouse(const char *p, RecompMouseConfig *m)
{
    char key[48], value[16];

    p = ws(p);
    if (*p != '{')
        return NULL;
    p = ws(p + 1);
    while (*p && *p != '}') {
        p = jstring(p, key, sizeof key);
        if (!p)
            return NULL;
        p = ws(p);
        if (*p != ':')
            return NULL;
        p = ws(p + 1);
        if (strcmp(key, "stick") == 0 && *p == '"') {
            p = jstring(p, value, sizeof value);
            if (!p)
                return NULL;
            m->stick = stick_by_name(value, m->stick);
        } else {
            if (strcmp(key, "port") == 0)
                m->port = (int)strtol(p, NULL, 10) - 1;      /* 1-based */
            else if (strcmp(key, "sensitivity") == 0)
                m->sensitivity = strtod(p, NULL);
            else if (strcmp(key, "invert_y") == 0)
                m->invert_y = jbool(p);
            else if (strcmp(key, "anti_deadzone") == 0)
                m->anti_deadzone = strtod(p, NULL);
            p = jskip(p);
            if (!p)
                return NULL;
        }
        p = ws(p);
        if (*p == ',')
            p = ws(p + 1);
    }
    return *p == '}' ? p + 1 : NULL;
}

static int parse_config(const char *text)
{
    const char *p = ws(text);
    char key[48];
    int bad = 0, index = 0;

    if (*p != '{')
        return 0;
    p = ws(p + 1);
    while (*p && *p != '}') {
        p = jstring(p, key, sizeof key);
        if (!p)
            return 0;
        p = ws(p);
        if (*p != ':')
            return 0;
        p = ws(p + 1);
        if (strcmp(key, "pad_api") == 0 && *p == '"') {
            char api[16];
            p = jstring(p, api, sizeof api);
            if (!p)
                return 0;
            recomp_pad_set_api(strcmp(api, "xinput") == 0 ? RECOMP_PAD_API_XINPUT
                                                          : RECOMP_PAD_API_SDL);
        } else if (strcmp(key, "mouse") == 0 && *p == '{') {
            p = parse_mouse(p, &g_mouse);
            if (!p)
                return 0;
        } else if (strcmp(key, "controllers") == 0 && *p == '[') {
            p = ws(p + 1);
            while (*p && *p != ']') {
                p = parse_controller(p, index++, &bad);
                if (!p)
                    return 0;
                p = ws(p);
                if (*p == ',')
                    p = ws(p + 1);
            }
            if (*p != ']')
                return 0;
            p++;
        } else {
            p = jskip(p);
            if (!p)
                return 0;
        }
        p = ws(p);
        if (*p == ',')
            p = ws(p + 1);
    }
    if (bad)
        fprintf(stderr, "[INPUT] %d binding(s) in the config file name a "
                        "control or a source this build does not know; "
                        "they are ignored\n", bad);
    return 1;
}

/* ---- where the config lives -------------------------------------------- */

static int readable(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return 0;
    fclose(f);
    return 1;
}

/* RECOMP_INPUT_CONFIG, then the per-user file the UI writes, then one beside
 * the executable (the working directory a title's run.bat starts in), so a
 * title can ship a config of its own. */
static int find_config(char *out, size_t n)
{
    const char *env = getenv("RECOMP_INPUT_CONFIG");
#if defined(_WIN32)
    const char *home = getenv("APPDATA");
    const char *tail = "\\xboxrecomp\\input_bindings.json";
#else
    const char *home = getenv("XDG_CONFIG_HOME");
    const char *tail = "/xboxrecomp/input_bindings.json";
    if (!home || !*home) {
        static char buf[400];
        const char *h = getenv("HOME");
        if (h) {
            snprintf(buf, sizeof buf, "%s/.config", h);
            home = buf;
        }
    }
#endif
    if (env && *env) {
        snprintf(out, n, "%s", env);
        if (readable(out))
            return 1;
        fprintf(stderr, "[INPUT] RECOMP_INPUT_CONFIG=%s cannot be read; "
                        "using the built-in bindings\n", env);
        return 0;
    }
    if (home && *home) {
        snprintf(out, n, "%s%s", home, tail);
        if (readable(out))
            return 1;
    }
    snprintf(out, n, "input_bindings.json");
    return readable(out);
}

static char *read_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    long size;
    char *buf;

    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size < 0 || size > (1 << 20)) {
        fclose(f);
        return NULL;
    }
    buf = (char *)malloc((size_t)size + 1);
    if (buf) {
        size_t got = fread(buf, 1, (size_t)size, f);
        buf[got] = '\0';
    }
    fclose(f);
    return buf;
}

static void describe(Controller *c, char *out, size_t n)
{
    const char *mouse = c->has_mouse ? " + mouse" : "";

    if (c->device == DEV_KEYBOARD)
        snprintf(out, n, "keyboard%s", mouse);
    else if (c->device == DEV_PAD && c->has_key)
        snprintf(out, n, "%s pad %d + keyboard%s", recomp_pad_api_name(), c->pad + 1, mouse);
    else if (c->device == DEV_PAD)
        snprintf(out, n, "%s pad %d%s", recomp_pad_api_name(), c->pad + 1, mouse);
    else
        snprintf(out, n, "nothing");
}

static void mouse_defaults(RecompMouseConfig *m)
{
    memset(m, 0, sizeof *m);
    m->stick = RECOMP_MOUSE_STICK_OFF;
    m->port = 0;
    m->sensitivity = 1.0;
    m->anti_deadzone = MOUSE_ANTI_DEADZONE_DEFAULT;
}

/* The environment over the file, as everywhere else in the runtime: a .bat
 * or a test run can switch mouse look on without touching the person's
 * bindings. An empty variable means on, the xbox_EnvSwitch rule. */
static void mouse_from_environment(RecompMouseConfig *m)
{
    const char *v;

    if ((v = getenv("RECOMP_MOUSE_STICK")) != NULL) {
        if (!*v || strcmp(v, "1") == 0 || strcmp(v, "on") == 0)
            m->stick = RECOMP_MOUSE_STICK_RIGHT;
        else if (strcmp(v, "0") == 0)
            m->stick = RECOMP_MOUSE_STICK_OFF;
        else
            m->stick = stick_by_name(v, m->stick);
    }
    if ((v = getenv("RECOMP_MOUSE_SENS")) != NULL && *v)
        m->sensitivity = strtod(v, NULL);
    if ((v = getenv("RECOMP_MOUSE_INVERT_Y")) != NULL)
        m->invert_y = !*v || jbool(v);
    if ((v = getenv("RECOMP_MOUSE_PORT")) != NULL && *v)
        m->port = (int)strtol(v, NULL, 10) - 1;
    if ((v = getenv("RECOMP_MOUSE_ANTI_DEADZONE")) != NULL && *v)
        m->anti_deadzone = strtod(v, NULL);
}

/* 0 not started, 1 loading, 2 loaded. The game window's thread asks for the
 * mouse settings while guest threads ask for the pads, so two threads can
 * arrive here together; the second waits for the first rather than reading
 * a half-parsed table. */
#if defined(_WIN32)
static volatile LONG g_init_state;
#else
static volatile long g_init_state;
#endif

static void load_bindings(void);

void recomp_bindings_init(void)
{
    if (g_init_state == 2)
        return;
#if defined(_WIN32)
    if (InterlockedCompareExchange(&g_init_state, 1, 0) != 0) {
        while (g_init_state != 2)
            Sleep(0);
        return;
    }
    load_bindings();
    InterlockedExchange(&g_init_state, 2);
#else
    if (!__sync_bool_compare_and_swap(&g_init_state, 0, 1)) {
        while (__atomic_load_n(&g_init_state, __ATOMIC_ACQUIRE) != 2)
            ;
        return;
    }
    load_bindings();
    __atomic_store_n(&g_init_state, 2, __ATOMIC_RELEASE);
#endif
}

static void load_bindings(void)
{
    char *text;
    int i;

    for (i = 0; i < PORTS; i++)
        defaults_for(&g_ctl[i], i);
    mouse_defaults(&g_mouse);
    if (find_config(g_path, sizeof g_path) && (text = read_file(g_path)) != NULL) {
        if (parse_config(text)) {
            g_have_path = 1;
        } else {
            fprintf(stderr, "[INPUT] %s is not a binding config this build "
                            "can read; using the built-in bindings\n", g_path);
            for (i = 0; i < PORTS; i++)
                defaults_for(&g_ctl[i], i);
            mouse_defaults(&g_mouse);
        }
        free(text);
    }
    mouse_from_environment(&g_mouse);
    if (g_mouse.port < 0 || g_mouse.port >= PORTS)
        g_mouse.port = 0;
    for (i = 0; i < PORTS; i++)
        if (g_ctl[i].has_mouse)
            g_mouse.buttons_bound = 1;
    if (g_mouse.stick != RECOMP_MOUSE_STICK_OFF)
        g_ctl[g_mouse.port].has_mouse = 1;
    recomp_mouse_configure(&g_mouse);
    for (i = 0; i < PORTS; i++)
        describe(&g_ctl[i], g_ctl[i].label, sizeof g_ctl[i].label);
    fprintf(stderr, "[INPUT] bindings from %s\n",
            g_have_path ? g_path : "the built-in defaults (no config file)");
    for (i = 0; i < PORTS; i++)
        fprintf(stderr, "[INPUT]   controller %d: %s\n", i + 1, g_ctl[i].label);
    fflush(stderr);
}

const char *recomp_bindings_source_path(void)
{
    recomp_bindings_init();
    return g_have_path ? g_path : NULL;
}

const char *recomp_bindings_device_name(unsigned port)
{
    recomp_bindings_init();
    return port < PORTS ? g_ctl[port].label : "nothing";
}

/* ---- reading the host -------------------------------------------------- */

static unsigned long long now_ms(void)
{
#if defined(_WIN32)
    return GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000ull + (unsigned long long)ts.tv_nsec / 1000000ull;
#endif
}

static const unsigned short PAD_BITS[PB_COUNT] = {
    RECOMP_PAD_A, RECOMP_PAD_B, RECOMP_PAD_X, RECOMP_PAD_Y,
    RECOMP_PAD_LEFT_SHOULDER, RECOMP_PAD_RIGHT_SHOULDER,
    RECOMP_PAD_START, RECOMP_PAD_BACK,
    RECOMP_PAD_LEFT_THUMB, RECOMP_PAD_RIGHT_THUMB,
    RECOMP_PAD_DPAD_UP, RECOMP_PAD_DPAD_DOWN,
    RECOMP_PAD_DPAD_LEFT, RECOMP_PAD_DPAD_RIGHT
};

/* One answer per key per sample: 24 controls can name the same key twice and
 * GetAsyncKeyState is a call into the window manager.
 *
 * Per sample is not enough on its own. A title is free to read the pad far
 * more often than it draws, and each read was costing a window-manager
 * transition per distinct key: Tony Hawk's Pro Skater 2X polls about 372,000
 * times a second and spent 47% of its main thread inside
 * NtUserGetAsyncKeyState -- four times what it spent in its own lifted code.
 *
 * So the answers also persist between samples for a short while. A keyboard
 * cannot change faster than a person can move, the console's own pad read is
 * a USB transfer that returns state captured up to a frame earlier, and one
 * millisecond is below anything a player can perceive. It is deliberately
 * not tied to the frame: a title that polls without drawing still has to get
 * fresh input eventually, or a menu that spins waiting for a key would never
 * see one.
 */
#define KEY_CACHE_TTL_MS 1u

typedef struct { unsigned char state[256]; } KeyCache;

static unsigned char g_key_state[256];
static unsigned long long g_key_state_at;

static int key_down(KeyCache *kc, int vk)
{
    (void)kc;
    if (vk <= 0 || vk > 255)
        return 0;
#if defined(_WIN32)
    if (!g_key_state[vk])
        g_key_state[vk] = (GetAsyncKeyState(vk) & 0x8000) ? 2 : 1;
    return g_key_state[vk] == 2;
#else
    return 0;           /* no keyboard source off Windows yet */
#endif
}

/* Start of a sample: drop answers older than the window above. */
static void key_cache_tick(void)
{
    unsigned long long now = now_ms();
    if (now - g_key_state_at >= KEY_CACHE_TTL_MS) {
        memset(g_key_state, 0, sizeof g_key_state);
        g_key_state_at = now;
    }
}

static int axis_value(const RecompPadState *g, int axis)
{
    switch (axis) {
    case AXIS_LX: return g->lx;
    case AXIS_LY: return g->ly;
    case AXIS_RX: return g->rx;
    default:      return g->ry;
    }
}

/* Every source reads as a 0..255 magnitude, whatever it is: that is what
 * makes a trigger bindable to a key and a button bindable to a stick. */
static int source_magnitude(const Source *s, KeyCache *kc,
                            const RecompPadState *pad, int have_pad, int deadzone)
{
    switch (s->kind) {
    case SRC_KEY:
        return key_down(kc, s->arg) ? 255 : 0;
    case SRC_PAD_BUTTON:
        if (!have_pad)
            return 0;
        return (pad->buttons & PAD_BITS[s->arg]) ? 255 : 0;
    case SRC_PAD_TRIGGER: {
        /* A resting trigger can read a few units above zero, and a title
         * that treats any non-zero analog button as pressed would fire
         * forever. Below XInput's own recommended threshold is "not
         * pressed"; above it the value passes through untouched. */
        int t;
        if (!have_pad)
            return 0;
        t = s->arg ? pad->right_trigger : pad->left_trigger;
        return t < TRIGGER_THRESHOLD ? 0 : t;
    }
    case SRC_MOUSE:
        return recomp_mouse_source(s->arg);
    case SRC_PAD_AXIS: {
        int v, span;
        if (!have_pad)
            return 0;
        v = axis_value(pad, s->arg) * s->sign;
        if (v <= deadzone)
            return 0;
        if (v > 32767)
            v = 32767;
        span = 32767 - deadzone;
        if (span <= 0)
            return 255;
        return (v - deadzone) * 255 / span;
    }
    default:
        return 0;
    }
}

/* The first thing pressed on a port, named once. A binding that is wrong is
 * otherwise invisible: the title just never responds, and there is no way to
 * tell a mis-bound key from a title that is not reading the pad at all. */
static void say_first_press(unsigned port, const char *control)
{
    static int said[PORTS];

    if (port < PORTS && !said[port]) {
        char name[64];
        said[port] = 1;
        name[0] = '\0';
        if (g_ctl[port].device == DEV_PAD)
            recomp_pad_name(g_ctl[port].pad, name, sizeof name);
        fprintf(stderr, "[INPUT] port %u first press: %s (%s%s%s)\n", port + 1,
                control, g_ctl[port].label, name[0] ? ": " : "", name);
        fflush(stderr);
    }
}

int recomp_bindings_sample(unsigned port, XBOX_GAMEPAD *out)
{
    Controller *c;
    KeyCache kc;
    RecompPadState pad;
    int have_pad = 0, i;
    int axis[4] = { 0, 0, 0, 0 };
    int mouse_axis[4] = { 0, 0, 0, 0 };

    if (!out)
        return 0;
    recomp_bindings_init();
    memset(out, 0, sizeof *out);
    if (port >= PORTS)
        return 0;
    c = &g_ctl[port];
    if (c->device == DEV_NONE)
        return 0;
    memset(&kc, 0, sizeof kc);
    key_cache_tick();
    memset(&pad, 0, sizeof pad);
    if (c->device == DEV_PAD)
        have_pad = recomp_pad_read(c->pad, &pad);

    for (i = 0; i < CONTROL_COUNT; i++) {
        int mag = 0, s;

        for (s = 0; s < c->bind[i].count; s++) {
            int m = source_magnitude(&c->bind[i].src[s], &kc, &pad, have_pad,
                                     c->deadzone);
            if (m > mag)
                mag = m;
        }
        if (!mag)
            continue;
        say_first_press(port, CONTROLS[i].name);
        switch (CONTROLS[i].kind) {
        case K_DIGITAL:
            if (mag >= 64)
                out->wButtons |= CONTROLS[i].arg;
            break;
        case K_ANALOG:
            if (mag > out->bAnalogButtons[CONTROLS[i].arg])
                out->bAnalogButtons[CONTROLS[i].arg] = (BYTE)mag;
            break;
        default:
            axis[CONTROLS[i].arg] += CONTROLS[i].sign * mag;
            break;
        }
    }
    /* Mouse look, on top of whatever the stick's own bindings say: a pad
     * stick and the mouse can both turn the camera, and the sum is clamped
     * like any two sources on one axis. It keeps the full 16-bit resolution
     * rather than the 0..255 every other source reads as. */
    if (c->has_mouse) {
        int mx, my, which = recomp_mouse_stick(port, &mx, &my);

        if (which) {
            int ax = which == RECOMP_MOUSE_STICK_LEFT ? AXIS_LX : AXIS_RX;
            int ay = which == RECOMP_MOUSE_STICK_LEFT ? AXIS_LY : AXIS_RY;

            mouse_axis[ax] = mx;
            mouse_axis[ay] = my;
            if (mx || my)
                say_first_press(port, which == RECOMP_MOUSE_STICK_LEFT
                                      ? "left stick (mouse)" : "right stick (mouse)");
        }
    }
    for (i = 0; i < 4; i++) {
        long v;
        if (axis[i] > 255)  axis[i] = 255;
        if (axis[i] < -255) axis[i] = -255;
        v = (long)axis[i] * 32767 / 255 + mouse_axis[i];
        if (v > 32767)  v = 32767;
        if (v < -32767) v = -32767;
        switch (i) {
        case AXIS_LX: out->sThumbLX = (SHORT)v; break;
        case AXIS_LY: out->sThumbLY = (SHORT)v; break;
        case AXIS_RX: out->sThumbRX = (SHORT)v; break;
        default:      out->sThumbRY = (SHORT)v; break;
        }
    }
    return 1;
}

unsigned recomp_bindings_present_mask(void)
{
    static unsigned long long next;
    static unsigned mask;
    unsigned long long now;
    int i;

    recomp_bindings_init();
    now = now_ms();
    if (mask && now < next)
        return mask;
    next = now + 250;
    mask = 0;
    for (i = 0; i < PORTS; i++) {
        Controller *c = &g_ctl[i];
        if (c->device == DEV_KEYBOARD ||
            (c->device == DEV_PAD && (c->has_key || c->has_mouse))) {
            mask |= 1u << i;
        } else if (c->device == DEV_PAD) {
            RecompPadState st;
            if (recomp_pad_read(c->pad, &st))
                mask |= 1u << i;
        }
    }
    return mask;
}

void recomp_bindings_rumble(unsigned port, unsigned short low, unsigned short high)
{
    recomp_bindings_init();
    if (port < PORTS && g_ctl[port].device == DEV_PAD)
        recomp_pad_rumble(g_ctl[port].pad, low, high);
}
