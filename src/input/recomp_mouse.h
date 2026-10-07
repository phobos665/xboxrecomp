/*
 * recomp_mouse.h -- the host mouse, as one controller's stick and as buttons
 * a binding can name.
 *
 * Two things a player asks for and the Xbox never had:
 *
 *   - Mouse look. Mouse movement drives one controller's stick (the right
 *     one, normally: it is the camera in nearly every title that has one).
 *     A mouse reports how far it moved, a stick reports where it is held,
 *     so the movement is turned into a deflection that follows the mouse's
 *     speed and falls back to centre when it stops -- see recomp_mouse.c.
 *
 *   - Mouse buttons as sources. "mouse:left", "mouse:wheel_up" and the rest
 *     bind to any control exactly as a key does (input_bindings.c).
 *
 * Both are read from raw input on the game's own window, which is why the
 * window has to hand its messages over (recomp_mouse_attach and
 * recomp_mouse_window_message, from src/hle/hle_d3d8.c). Raw input arrives
 * only while that window is in front, so a click in another program never
 * reaches the title.
 *
 * Nothing here runs unless the binding config asks for it: with mouse look
 * off and no control bound to a mouse source, attach registers nothing and
 * the window's messages pass straight through.
 */
#ifndef XBOXRECOMP_RECOMP_MOUSE_H
#define XBOXRECOMP_RECOMP_MOUSE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Which stick the mouse moves. */
enum {
    RECOMP_MOUSE_STICK_OFF   = 0,
    RECOMP_MOUSE_STICK_LEFT  = 1,
    RECOMP_MOUSE_STICK_RIGHT = 2
};

/* The mouse sources a binding can name, in the order MOUSE_SOURCES in
 * input_bindings.c spells them. Left and right are the primary and
 * secondary buttons as Windows has them, so a left-handed mouse setting
 * carries over. */
enum {
    RECOMP_MOUSE_LEFT, RECOMP_MOUSE_RIGHT, RECOMP_MOUSE_MIDDLE,
    RECOMP_MOUSE_X1, RECOMP_MOUSE_X2,
    RECOMP_MOUSE_WHEEL_UP, RECOMP_MOUSE_WHEEL_DOWN,
    RECOMP_MOUSE_SOURCE_COUNT
};

typedef struct RecompMouseConfig {
    int    stick;           /* RECOMP_MOUSE_STICK_* */
    int    port;            /* 0..3: the controller whose stick it moves */
    double sensitivity;     /* 1.0: full deflection at ~2500 counts a second */
    int    invert_y;        /* mouse forward looks down */
    double anti_deadzone;   /* 0..0.9: smallest deflection a movement gives */
    int    buttons_bound;   /* some control names a mouse: source */
} RecompMouseConfig;

/* Set by recomp_bindings_init() from the config file and the RECOMP_MOUSE_*
 * environment variables. Everything else reads it. */
void recomp_mouse_configure(const RecompMouseConfig *cfg);
const RecompMouseConfig *recomp_mouse_config(void);

/* Mouse look is on, or some control is bound to a mouse button. */
int  recomp_mouse_wanted(void);

/* The game window, from the thread that owns it, before it is first shown:
 * registers for raw mouse input if the config wants the mouse at all, and
 * says once in the log what the mouse will do. */
void recomp_mouse_attach(void *hwnd);

/* Called first thing in that window's procedure. Returns 1 when the message
 * was answered here (and *result is what to return), 0 to carry on as if
 * this had not been called. WM_INPUT always returns 0: DefWindowProc has to
 * see it to free the input. Off Windows this does nothing. */
int  recomp_mouse_window_message(void *hwnd, unsigned msg, uintptr_t wp,
                                 intptr_t lp, intptr_t *result);

/* Movement in mouse counts, x right and y down, as raw input reports it.
 * The window feeds this while the mouse is captured; it is public so a test
 * or another host backend can feed it too. */
void recomp_mouse_feed_motion(long dx, long dy);

/* The mouse's contribution to `port`'s stick, -32767..32767 with y up as a
 * stick has it. Returns which stick (RECOMP_MOUSE_STICK_*), or 0 when mouse
 * look is off or belongs to another port. Safe from any thread. */
int  recomp_mouse_stick(unsigned port, int *x, int *y);

/* One mouse source as a 0..255 magnitude, like any other source. A wheel
 * notch reads as a short press, then a short release, one per notch, so a
 * title that acts on the press edge sees every notch. */
int  recomp_mouse_source(int which);

#ifdef __cplusplus
}
#endif

#endif
