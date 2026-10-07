/*
 * recomp_mouse.c -- see recomp_mouse.h.
 *
 * Two threads meet here. The game window's thread receives WM_INPUT and adds
 * each movement to a pair of interlocked accumulators, and sets or clears a
 * bit per button; it never waits for anything, because a window thread that
 * blocks stalls the Present that is waiting on it. Guest threads, polling
 * the pads, drain the accumulators into the stick model under a small lock
 * -- small because several guest threads may poll the same port, and the
 * model's state has to move forward once per poll, not once per thread.
 *
 * The stick model. A mouse says how far it moved since it last said
 * anything; a stick says where it is held, and a title turns that into a
 * turning *rate*. So the deflection follows the mouse's speed: each poll the
 * deflection decays towards centre with a time constant of MOUSE_TAU, and
 * the counts that arrived since the last poll are added on top. Done
 * naively that ties the result to the poll rate -- a title that polls 300,000
 * times a second would see a few counts per poll and almost no deflection --
 * so the counts are treated as spread evenly over the interval since the
 * previous poll, which makes a steady mouse speed v give a steady deflection
 * of v / MOUSE_FULL_RATE * sensitivity whether the title polls once a frame
 * or continuously. When the mouse stops, the stick is back at rest within a
 * few MOUSE_TAU; a flick saturates and is clamped (radially, as a stick's
 * gate is round) so it does not keep turning after the hand has stopped.
 *
 * A title applies its own deadzone to the stick it reads -- usually around a
 * quarter of the travel -- and a slow mouse movement would vanish inside it.
 * The anti-deadzone lifts any deflection that is not at rest to at least that
 * fraction, so the first count of movement turns the camera.
 *
 * Capture. While mouse look is on and the window is in front, the cursor is
 * clipped to the picture and hidden, because a cursor that wanders off the
 * window takes the input focus with it at the first click. F8 lets it go
 * (F9, F10 and F11 are the frame-rate overlay, the frame cap and the frame
 * capture; F12 breaks into an attached debugger); a click on the picture,
 * or coming back to the window, takes it again. The click that does so is
 * not passed to the title.
 */
#include "recomp_mouse.h"
#include "input_bindings.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
typedef volatile LONG mouse_atomic;
#  define ATOMIC_ADD(p, v)     InterlockedExchangeAdd((p), (v))
#  define ATOMIC_XCHG(p, v)    InterlockedExchange((p), (v))
#  define ATOMIC_CAS(p, n, o)  InterlockedCompareExchange((p), (n), (o))
#  define ATOMIC_OR(p, v)      InterlockedOr((p), (v))
#  define ATOMIC_AND(p, v)     InterlockedAnd((p), (v))
static SRWLOCK g_lock = SRWLOCK_INIT;
#  define LOCK()   AcquireSRWLockExclusive(&g_lock)
#  define UNLOCK() ReleaseSRWLockExclusive(&g_lock)
#else
#  include <pthread.h>
#  include <time.h>
typedef volatile long mouse_atomic;
#  define ATOMIC_ADD(p, v)     __atomic_fetch_add((p), (v), __ATOMIC_SEQ_CST)
#  define ATOMIC_XCHG(p, v)    __atomic_exchange_n((p), (v), __ATOMIC_SEQ_CST)
#  define ATOMIC_CAS(p, n, o)  __sync_val_compare_and_swap((p), (o), (n))
#  define ATOMIC_OR(p, v)      __atomic_fetch_or((p), (v), __ATOMIC_SEQ_CST)
#  define ATOMIC_AND(p, v)     __atomic_fetch_and((p), (v), __ATOMIC_SEQ_CST)
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
#  define LOCK()   pthread_mutex_lock(&g_lock)
#  define UNLOCK() pthread_mutex_unlock(&g_lock)
#endif

/* How fast the stick returns to centre once the mouse stops: after one
 * MOUSE_TAU it is at 37%, after four at 2%. Shorter feels twitchy at a low
 * poll rate, longer feels like the camera is on ice. */
#define MOUSE_TAU        0.040
/* The mouse speed, in counts a second, that holds the stick fully over at
 * sensitivity 1.0: a slow sweep with an 800 dpi mouse, a gentle one at
 * 1600. */
#define MOUSE_FULL_RATE  2500.0
/* Below this the stick reads as at rest: otherwise the anti-deadzone would
 * hold the camera turning through the decay's long tail. */
#define MOUSE_REST       0.02
/* A wheel notch is held this long, then let go for MOUSE_WHEEL_GAP before
 * the next one: long enough for a title polling once a frame to see both. */
#define MOUSE_WHEEL_PRESS 0.060
#define MOUSE_WHEEL_GAP   0.040
#define MOUSE_WHEEL_QUEUE 4
/* The key that lets the cursor go. */
#define MOUSE_RELEASE_VK  0x77          /* VK_F8 */

static RecompMouseConfig g_cfg;

static mouse_atomic g_acc_x, g_acc_y;   /* counts not yet taken by a poll */
static mouse_atomic g_buttons;          /* bit per RECOMP_MOUSE_LEFT..X2 */
static mouse_atomic g_wheel_pending[2]; /* notches not yet pressed: up, down */
static mouse_atomic g_captured;

/* Under g_lock. */
static double g_sx, g_sy;               /* the deflection, -1..1, y down */
static double g_last_poll;
static struct { int pressed; double until; } g_wheel[2];

static double now_seconds(void)
{
#if defined(_WIN32)
    static LARGE_INTEGER freq;
    LARGE_INTEGER t;

    if (!freq.QuadPart)
        QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart / (double)freq.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
#endif
}

void recomp_mouse_configure(const RecompMouseConfig *cfg)
{
    if (!cfg)
        return;
    g_cfg = *cfg;
    if (g_cfg.port < 0 || g_cfg.port > 3)
        g_cfg.port = 0;
    if (!(g_cfg.sensitivity > 0.0))
        g_cfg.sensitivity = 1.0;
    if (g_cfg.anti_deadzone < 0.0)
        g_cfg.anti_deadzone = 0.0;
    if (g_cfg.anti_deadzone > 0.9)
        g_cfg.anti_deadzone = 0.9;
}

const RecompMouseConfig *recomp_mouse_config(void)
{
    return &g_cfg;
}

int recomp_mouse_wanted(void)
{
    recomp_bindings_init();
    return g_cfg.stick != RECOMP_MOUSE_STICK_OFF || g_cfg.buttons_bound;
}

void recomp_mouse_feed_motion(long dx, long dy)
{
    if (dx)
        ATOMIC_ADD(&g_acc_x, dx);
    if (dy)
        ATOMIC_ADD(&g_acc_y, dy);
}

int recomp_mouse_stick(unsigned port, int *x, int *y)
{
    double now, dt, decay, spread, gain, mag, out;
    long dx, dy;

    if (x) *x = 0;
    if (y) *y = 0;
    if (g_cfg.stick == RECOMP_MOUSE_STICK_OFF || port != (unsigned)g_cfg.port)
        return 0;

    LOCK();
    now = now_seconds();
    dt = g_last_poll > 0.0 ? now - g_last_poll : MOUSE_TAU;
    /* A title that stops polling for a while (a load, a pause) comes back
     * to a stick at rest, not to a deflection from before it stopped. */
    if (dt > 0.25)
        dt = 0.25;
    dx = ATOMIC_XCHG(&g_acc_x, 0);
    dy = ATOMIC_XCHG(&g_acc_y, 0);
    if (dt < 1e-4) {
        /* Polls closer together than this (a title spinning on its pad)
         * land as an impulse -- the limit of the spread below as dt goes
         * to zero -- and the clock is not moved on, so the decay they skip
         * is applied in full at the next poll that is far enough apart.
         * Moving it on would lose that time and the stick would never come
         * back down. */
        decay = 1.0;
        spread = 1.0 / (MOUSE_FULL_RATE * MOUSE_TAU);
    } else {
        g_last_poll = now;
        decay = exp(-dt / MOUSE_TAU);
        /* Counts spread over dt, integrated through the same decay: in the
         * steady state this is exactly speed / MOUSE_FULL_RATE. */
        spread = (1.0 - decay) / (dt * MOUSE_FULL_RATE);
    }
    gain = spread * g_cfg.sensitivity;
    g_sx = g_sx * decay + (double)dx * gain;
    g_sy = g_sy * decay + (double)dy * gain;

    mag = sqrt(g_sx * g_sx + g_sy * g_sy);
    if (mag > 1.0) {
        g_sx /= mag;
        g_sy /= mag;
        mag = 1.0;
    }
    if (mag < MOUSE_REST) {
        /* Reads as centred, but the state is kept: a title polling every
         * few microseconds sees a count at a time, and each one adds less
         * than this -- zeroing here would stop the stick ever moving. */
        UNLOCK();
        return g_cfg.stick;
    }
    out = g_cfg.anti_deadzone + (1.0 - g_cfg.anti_deadzone) * mag;
    if (x) *x = (int)lround(g_sx / mag * out * 32767.0);
    /* A mouse's y grows downward; a stick's grows upward. Pushing the mouse
     * away looks up, as it does in every PC game, unless inverted. */
    if (y) *y = (int)lround((g_cfg.invert_y ? 1.0 : -1.0) * g_sy / mag * out * 32767.0);
    UNLOCK();
    return g_cfg.stick;
}

/* One wheel direction, stepped forward to `now`: release a press that has
 * been held long enough, start the next queued notch once the gap is over. */
static int wheel_read(int dir, double now)
{
    if (g_wheel[dir].pressed && now >= g_wheel[dir].until) {
        g_wheel[dir].pressed = 0;
        g_wheel[dir].until = now + MOUSE_WHEEL_GAP;
    }
    if (!g_wheel[dir].pressed && now >= g_wheel[dir].until) {
        long n = g_wheel_pending[dir];

        if (n > 0 && ATOMIC_CAS(&g_wheel_pending[dir], n - 1, n) == n) {
            g_wheel[dir].pressed = 1;
            g_wheel[dir].until = now + MOUSE_WHEEL_PRESS;
        }
    }
    return g_wheel[dir].pressed;
}

int recomp_mouse_source(int which)
{
    int on;

    if (which >= RECOMP_MOUSE_LEFT && which <= RECOMP_MOUSE_X2)
        return (g_buttons & (1L << which)) ? 255 : 0;
    if (which != RECOMP_MOUSE_WHEEL_UP && which != RECOMP_MOUSE_WHEEL_DOWN)
        return 0;
    LOCK();
    on = wheel_read(which - RECOMP_MOUSE_WHEEL_UP, now_seconds());
    UNLOCK();
    return on ? 255 : 0;
}

/* ---- the window -------------------------------------------------------- */

#if defined(_WIN32)

static HWND g_hwnd;
static int  g_attached;
static long g_suppress;                 /* window thread only */

static void wheel_notch(int dir, int notches)
{
    long n;

    if (notches < 1)
        notches = 1;
    n = ATOMIC_ADD(&g_wheel_pending[dir], notches) + notches;
    if (n > MOUSE_WHEEL_QUEUE)
        ATOMIC_XCHG(&g_wheel_pending[dir], MOUSE_WHEEL_QUEUE);
}

/* Physical button bits (raw input and GetAsyncKeyState both report the
 * physical buttons) as the logical ones the sources name. */
static long logical_buttons(long physical)
{
    if (GetSystemMetrics(SM_SWAPBUTTON)) {
        long l = physical & (1L << RECOMP_MOUSE_LEFT), r = physical & (1L << RECOMP_MOUSE_RIGHT);

        physical &= ~((1L << RECOMP_MOUSE_LEFT) | (1L << RECOMP_MOUSE_RIGHT));
        if (l) physical |= 1L << RECOMP_MOUSE_RIGHT;
        if (r) physical |= 1L << RECOMP_MOUSE_LEFT;
    }
    return physical;
}

static int cursor_in_client(HWND hwnd)
{
    POINT pt;
    RECT rc;

    if (!GetCursorPos(&pt) || !ScreenToClient(hwnd, &pt) || !GetClientRect(hwnd, &rc))
        return 0;
    return pt.x >= rc.left && pt.x < rc.right && pt.y >= rc.top && pt.y < rc.bottom;
}

static void clip_to_client(HWND hwnd)
{
    RECT rc;

    GetClientRect(hwnd, &rc);
    MapWindowPoints(hwnd, NULL, (POINT *)&rc, 2);
    ClipCursor(&rc);
}

/* Whatever is held now is the click that brought the window back, and the
 * title should not see it; its release clears it. */
static void suppress_held(void)
{
    long held = 0;

    if (GetAsyncKeyState(VK_LBUTTON) & 0x8000)  held |= 1L << RECOMP_MOUSE_LEFT;
    if (GetAsyncKeyState(VK_RBUTTON) & 0x8000)  held |= 1L << RECOMP_MOUSE_RIGHT;
    if (GetAsyncKeyState(VK_MBUTTON) & 0x8000)  held |= 1L << RECOMP_MOUSE_MIDDLE;
    if (GetAsyncKeyState(VK_XBUTTON1) & 0x8000) held |= 1L << RECOMP_MOUSE_X1;
    if (GetAsyncKeyState(VK_XBUTTON2) & 0x8000) held |= 1L << RECOMP_MOUSE_X2;
    g_suppress |= logical_buttons(held);
}

static void capture(HWND hwnd)
{
    static int said;

    if (g_cfg.stick == RECOMP_MOUSE_STICK_OFF || g_captured)
        return;
    if (GetForegroundWindow() != hwnd || IsIconic(hwnd))
        return;
    clip_to_client(hwnd);
    SetCursor(NULL);
    ATOMIC_XCHG(&g_acc_x, 0);
    ATOMIC_XCHG(&g_acc_y, 0);
    suppress_held();
    ATOMIC_XCHG(&g_captured, 1);
    if (!said) {
        said = 1;
        fprintf(stderr, "[INPUT] mouse captured: F8 lets the cursor go, a click "
                        "on the picture takes it back\n");
        fflush(stderr);
    }
}

static void release(void)
{
    if (!g_captured)
        return;
    ATOMIC_XCHG(&g_captured, 0);
    ClipCursor(NULL);
    ATOMIC_XCHG(&g_acc_x, 0);
    ATOMIC_XCHG(&g_acc_y, 0);
}

/* Everything let go: the window lost the focus, and the button-up messages
 * that would have cleared these now go to someone else. */
static void drop_buttons(void)
{
    ATOMIC_XCHG(&g_buttons, 0);
    g_suppress = 0;
}

static void raw_mouse(HWND hwnd, const RAWMOUSE *m)
{
    static const USHORT down[5] = {
        RI_MOUSE_BUTTON_1_DOWN, RI_MOUSE_BUTTON_2_DOWN, RI_MOUSE_BUTTON_3_DOWN,
        RI_MOUSE_BUTTON_4_DOWN, RI_MOUSE_BUTTON_5_DOWN
    };
    static const USHORT up[5] = {
        RI_MOUSE_BUTTON_1_UP, RI_MOUSE_BUTTON_2_UP, RI_MOUSE_BUTTON_3_UP,
        RI_MOUSE_BUTTON_4_UP, RI_MOUSE_BUTTON_5_UP
    };
    static LONG abs_x, abs_y;
    static int have_abs;
    const int look = g_cfg.stick != RECOMP_MOUSE_STICK_OFF;
    USHORT f = m->usButtonFlags;
    long dx, dy, went_down = 0, went_up = 0;
    int i;

    /* Remote desktop and some virtual machines report where the pointer
     * is, 0..65535 across the screen, rather than how far it moved. */
    if (m->usFlags & MOUSE_MOVE_ABSOLUTE) {
        int virt = (m->usFlags & MOUSE_VIRTUAL_DESKTOP) != 0;
        int w = GetSystemMetrics(virt ? SM_CXVIRTUALSCREEN : SM_CXSCREEN);
        int h = GetSystemMetrics(virt ? SM_CYVIRTUALSCREEN : SM_CYSCREEN);

        dx = have_abs ? (long)((double)(m->lLastX - abs_x) * w / 65535.0) : 0;
        dy = have_abs ? (long)((double)(m->lLastY - abs_y) * h / 65535.0) : 0;
        abs_x = m->lLastX;
        abs_y = m->lLastY;
        have_abs = 1;
    } else {
        dx = m->lLastX;
        dy = m->lLastY;
    }
    if (look && g_captured)
        recomp_mouse_feed_motion(dx, dy);

    for (i = 0; i < 5; i++) {
        if (f & down[i]) went_down |= 1L << i;
        if (f & up[i])   went_up   |= 1L << i;
    }
    went_down = logical_buttons(went_down);
    went_up = logical_buttons(went_up);

    if (went_up) {
        ATOMIC_AND(&g_buttons, ~went_up);
        g_suppress &= ~went_up;
    }
    if (went_down) {
        if (look && !g_captured) {
            /* A click on the picture takes the mouse back, and is only
             * that: the title does not see it. */
            if (cursor_in_client(hwnd)) {
                capture(hwnd);
                g_suppress |= went_down;
            }
            went_down = 0;
        } else if (!look && !cursor_in_client(hwnd)) {
            went_down = 0;              /* the title bar, a border */
        }
        went_down &= ~g_suppress;
        if (went_down)
            ATOMIC_OR(&g_buttons, went_down);
    }

    if (f & RI_MOUSE_WHEEL) {
        SHORT delta = (SHORT)m->usButtonData;

        if (delta && (look ? g_captured != 0 : cursor_in_client(hwnd)))
            wheel_notch(delta > 0 ? 0 : 1, (delta < 0 ? -delta : delta) / WHEEL_DELTA);
    }
}

void recomp_mouse_attach(void *hwnd_ptr)
{
    HWND hwnd = (HWND)hwnd_ptr;
    RAWINPUTDEVICE rid;
    const RecompMouseConfig *c;

    if (!hwnd || g_attached || !recomp_mouse_wanted())
        return;
    c = &g_cfg;
    /* Usage page 1 (generic desktop), usage 2 (mouse). No RIDEV_INPUTSINK:
     * input arrives only while this window is in front, which is the rule
     * a player expects. Not RIDEV_NOLEGACY either: the window still gets
     * its ordinary mouse messages, for the title bar and the frame. */
    memset(&rid, 0, sizeof rid);
    rid.usUsagePage = 0x01;
    rid.usUsage = 0x02;
    rid.hwndTarget = hwnd;
    if (!RegisterRawInputDevices(&rid, 1, sizeof rid)) {
        fprintf(stderr, "[INPUT] mouse: RegisterRawInputDevices failed (%lu); "
                        "the mouse will do nothing\n", GetLastError());
        fflush(stderr);
        return;
    }
    g_hwnd = hwnd;
    g_attached = 1;
    if (c->stick != RECOMP_MOUSE_STICK_OFF)
        fprintf(stderr, "[INPUT] mouse moves controller %d's %s stick (sensitivity "
                        "%.2f, %s, anti-deadzone %.2f)%s\n", c->port + 1,
                c->stick == RECOMP_MOUSE_STICK_LEFT ? "left" : "right",
                c->sensitivity, c->invert_y ? "Y inverted" : "Y not inverted",
                c->anti_deadzone, c->buttons_bound ? "; its buttons are bound too" : "");
    else
        fprintf(stderr, "[INPUT] mouse buttons are bound; read from the game window\n");
    fflush(stderr);
}

int recomp_mouse_window_message(void *hwnd_ptr, unsigned msg, uintptr_t wp,
                                intptr_t lp, intptr_t *result)
{
    HWND hwnd = (HWND)hwnd_ptr;

    if (!g_attached || hwnd != g_hwnd)
        return 0;
    switch (msg) {
    case WM_INPUT: {
        RAWINPUT ri;
        UINT size = sizeof ri;

        if (GetRawInputData((HRAWINPUT)lp, RID_INPUT, &ri, &size,
                            sizeof(RAWINPUTHEADER)) != (UINT)-1 &&
            ri.header.dwType == RIM_TYPEMOUSE)
            raw_mouse(hwnd, &ri.data.mouse);
        return 0;                       /* DefWindowProc frees the input */
    }
    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE) {
            release();
            drop_buttons();
        } else if (!HIWORD(wp)) {
            if (LOWORD(wp) == WA_CLICKACTIVE)
                suppress_held();        /* a click to focus is not a shot */
            /* Back by Alt+Tab, or by a click on the picture. A click on the
             * title bar is someone moving the window: leave the cursor be. */
            if (LOWORD(wp) == WA_ACTIVE || cursor_in_client(hwnd))
                capture(hwnd);
        }
        return 0;
    case WM_KILLFOCUS:
        release();
        drop_buttons();
        return 0;
    case WM_SIZE:
    case WM_MOVE:
        if (g_captured)
            clip_to_client(hwnd);       /* Alt+Enter, or the window moved */
        return 0;
    case WM_SETCURSOR:
        if (g_captured && LOWORD(lp) == HTCLIENT) {
            SetCursor(NULL);
            if (result)
                *result = TRUE;
            return 1;
        }
        return 0;
    case WM_KEYDOWN:
        if (wp == MOUSE_RELEASE_VK && !(lp & (1 << 30)) &&
            g_cfg.stick != RECOMP_MOUSE_STICK_OFF) {
            if (g_captured)
                release();
            else
                capture(hwnd);
            if (result)
                *result = 0;
            return 1;
        }
        return 0;
    case WM_DESTROY:
        release();
        return 0;
    default:
        return 0;
    }
}

#else   /* no window backend off Windows yet */

void recomp_mouse_attach(void *hwnd)
{
    (void)hwnd;
}

int recomp_mouse_window_message(void *hwnd, unsigned msg, uintptr_t wp,
                                intptr_t lp, intptr_t *result)
{
    (void)hwnd; (void)msg; (void)wp; (void)lp; (void)result;
    return 0;
}

#endif
