/*
 * host_timer.c - see host_timer.h.
 *
 * The Windows half is the code the three callers carried, moved here
 * unchanged in effect: a CREATE_WAITABLE_TIMER_HIGH_RESOLUTION timer armed
 * with a relative due time in 100 ns units, waited on with
 * WaitForSingleObject / WaitForMultipleObjects.
 */
#include "host_timer.h"

#include <stdlib.h>

#ifdef _WIN32
/* ===================================================================== */
/* Windows                                                               */
/* ===================================================================== */
#include <windows.h>
#include <mmsystem.h>   /* timeBeginPeriod */

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

struct host_timer {
    HANDLE handle;
    int    high_res;
};

host_timer *host_timer_create(int kind)
{
    host_timer *t;
    HANDLE h = CreateWaitableTimerExW(NULL, NULL,
                                      CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                      TIMER_ALL_ACCESS);
    int high_res = h != NULL;

    if (!h && kind == HOST_TIMER_ANY)
        h = CreateWaitableTimerW(NULL, FALSE, NULL);
    if (!h)
        return NULL;
    t = (host_timer *)malloc(sizeof *t);
    if (!t) {
        CloseHandle(h);
        return NULL;
    }
    t->handle = h;
    t->high_res = high_res;
    return t;
}

void host_timer_destroy(host_timer *t)
{
    if (!t)
        return;
    CloseHandle(t->handle);
    free(t);
}

int host_timer_high_res(const host_timer *t)
{
    return t && t->high_res;
}

int64_t host_time_ns(void)
{
    static LONGLONG qpf;
    LARGE_INTEGER now;

    if (!qpf) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        qpf = f.QuadPart;
    }
    QueryPerformanceCounter(&now);
    /* Split so the multiply cannot overflow at a 10 MHz counter. */
    return (int64_t)(now.QuadPart / qpf) * 1000000000 +
           (int64_t)(now.QuadPart % qpf) * 1000000000 / qpf;
}

/* Relative, in 100 ns units. */
static int arm(host_timer *t, int64_t us)
{
    LARGE_INTEGER due;

    due.QuadPart = -(LONGLONG)us * 10;
    return SetWaitableTimer(t->handle, &due, 0, NULL, NULL, FALSE)
               ? HOST_WAIT_ELAPSED : HOST_WAIT_NOT_ARMED;
}

int host_timer_wait_us(host_timer *t, int64_t us, uint32_t cap_ms)
{
    if (!t || arm(t, us) != HOST_WAIT_ELAPSED)
        return HOST_WAIT_NOT_ARMED;
    return WaitForSingleObject(t->handle, cap_ms) == WAIT_OBJECT_0
               ? HOST_WAIT_ELAPSED : HOST_WAIT_FAILED;
}

int host_timer_wait_until(host_timer *t, int64_t deadline_ns, uint32_t cap_ms)
{
    int64_t left = deadline_ns - host_time_ns();

    if (!t)
        return HOST_WAIT_NOT_ARMED;
    if (left <= 0)
        return HOST_WAIT_ELAPSED;
    return host_timer_wait_us(t, (left + 999) / 1000, cap_ms);
}

int host_timer_wait_us_or_event(host_timer *t, int64_t us, void *event, uint32_t cap_ms)
{
    HANDLE both[2];
    DWORD r;

    if (!t || arm(t, us) != HOST_WAIT_ELAPSED)
        return HOST_WAIT_NOT_ARMED;
    both[0] = (HANDLE)event;
    both[1] = t->handle;
    r = WaitForMultipleObjects(2, both, FALSE, cap_ms);
    if (r == WAIT_OBJECT_0) {
        CancelWaitableTimer(t->handle);
        return HOST_WAIT_EVENT;
    }
    return r == WAIT_OBJECT_0 + 1 ? HOST_WAIT_ELAPSED : HOST_WAIT_FAILED;
}

void host_sleep_precision_1ms(void)
{
    timeBeginPeriod(1);
}

#else
/* ===================================================================== */
/* POSIX                                                                 */
/* ===================================================================== */
#include <errno.h>
#include <time.h>
#ifdef __APPLE__
#include <mach/mach_time.h>
#endif
#include "win32_compat.h"   /* w32_wait_single_us, for the event wait */

/* Nothing to hold: the clock is the timer. The struct exists so the API is
 * the same everywhere and a caller's NULL check means what it does on
 * Windows. */
struct host_timer {
    int unused;
};

host_timer *host_timer_create(int kind)
{
    (void)kind;
    return (host_timer *)calloc(1, sizeof(host_timer));
}

void host_timer_destroy(host_timer *t)
{
    free(t);
}

int host_timer_high_res(const host_timer *t)
{
    return t != NULL;
}

/* macOS adds leeway to every timed wait of a thread that is not real-time:
 * measured on an M4 (macOS 27), a wait of N wakes N/4 late, whatever the
 * call (mach_wait_until, nanosleep, a condition variable) and whatever the
 * QoS class -- 8 ms woke 2.0 ms late, which would cost the vblank clock an
 * eighth of a frame. Only THREAD_TIME_CONSTRAINT_POLICY removes it, and a
 * real-time thread that computes past its budget is demoted, so that is no
 * answer for the guest's own thread, which is where the flip gate waits.
 *
 * So a wait here is a series of shorter ones: each asks for 3/4 of what
 * remains, so even woken a quarter late it lands short of the deadline, and
 * the last ~200 us is spun. From 10 ms that is four sleeps and a spin of at
 * most a fifth of a millisecond, and the wake is within a few microseconds.
 * Linux's timer slack is 50 us, so there one sleep does it. */
#ifdef __APPLE__
#define WAIT_STEP(left_ns)  ((left_ns) * 3 / 4)
#define WAIT_SPIN_NS        200000
#else
#define WAIT_STEP(left_ns)  (left_ns)
#define WAIT_SPIN_NS        0
#endif

#ifdef __APPLE__
static mach_timebase_info_data_t g_timebase;

int64_t host_time_ns(void)
{
    return (int64_t)clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
}

/* mach_wait_until sleeps to an absolute mach_absolute_time, the clock
 * CLOCK_UPTIME_RAW reads. */
static void sleep_until_ns(int64_t deadline_ns)
{
    uint64_t ticks;

    if (!g_timebase.denom)
        mach_timebase_info(&g_timebase);
    /* ns -> ticks: ns * denom / numer (1/1 on Intel, 125/3 on Apple
     * Silicon). Split to keep the multiply in range. */
    ticks = (uint64_t)(deadline_ns / g_timebase.numer) * g_timebase.denom +
            (uint64_t)(deadline_ns % g_timebase.numer) * g_timebase.denom /
                g_timebase.numer;
    while (mach_wait_until(ticks) == KERN_ABORTED)
        ;
}
#else
int64_t host_time_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

static void sleep_until_ns(int64_t deadline_ns)
{
    struct timespec ts;

    ts.tv_sec  = (time_t)(deadline_ns / 1000000000);
    ts.tv_nsec = (long)(deadline_ns % 1000000000);
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) == EINTR)
        ;
}
#endif

/* To the deadline, as precisely as the host allows (the comment above). */
static void sleep_precisely_until(int64_t deadline_ns)
{
    int64_t left;

    while ((left = deadline_ns - host_time_ns()) > WAIT_SPIN_NS)
        sleep_until_ns(deadline_ns - left + WAIT_STEP(left));
    while (host_time_ns() < deadline_ns)
        ;
}

int host_timer_wait_until(host_timer *t, int64_t deadline_ns, uint32_t cap_ms)
{
    int64_t now;

    if (!t)
        return HOST_WAIT_NOT_ARMED;
    now = host_time_ns();
    if (deadline_ns <= now)
        return HOST_WAIT_ELAPSED;
    if (cap_ms != 0xFFFFFFFFu && deadline_ns - now > (int64_t)cap_ms * 1000000) {
        sleep_precisely_until(now + (int64_t)cap_ms * 1000000);
        return HOST_WAIT_FAILED;
    }
    sleep_precisely_until(deadline_ns);
    return HOST_WAIT_ELAPSED;
}

int host_timer_wait_us(host_timer *t, int64_t us, uint32_t cap_ms)
{
    if (us < 0)
        us = 0;
    return host_timer_wait_until(t, host_time_ns() + us * 1000, cap_ms);
}

int host_timer_wait_us_or_event(host_timer *t, int64_t us, void *event, uint32_t cap_ms)
{
    int capped = 0;
    int64_t deadline, left;

    if (!t)
        return HOST_WAIT_NOT_ARMED;
    if (us < 0)
        us = 0;
    if (cap_ms != 0xFFFFFFFFu && us > (int64_t)cap_ms * 1000) {
        us = (int64_t)cap_ms * 1000;
        capped = 1;
    }
    deadline = host_time_ns() + us * 1000;
    /* Condition-variable waits on the event, stepped down as above: the
     * event is a win32_compat object, so its SetEvent wakes any of them at
     * once. The spin at the end polls it. */
    for (;;) {
        DWORD r;

        left = deadline - host_time_ns();
        if (left > WAIT_SPIN_NS)
            r = w32_wait_single_us((HANDLE)event, WAIT_STEP(left) / 1000);
        else
            r = w32_wait_single_us((HANDLE)event, 0);
        if (r == WAIT_OBJECT_0)
            return HOST_WAIT_EVENT;
        if (r != WAIT_TIMEOUT)
            return HOST_WAIT_FAILED;
        if (left <= 0)
            return capped ? HOST_WAIT_FAILED : HOST_WAIT_ELAPSED;
    }
}

void host_sleep_precision_1ms(void)
{
}

#endif
