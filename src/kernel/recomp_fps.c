/**
 * Frame rate, measured at the one place every configuration shares.
 *
 * Every counter the runtime had was per subsystem -- shadow swaps, executor
 * flips, pushbuffer segments -- so two configurations could not be compared on
 * the number that matters. This counts the title's own D3DDevice_Swap at the
 * HLE boundary, before the replacement does anything, so it means the same
 * thing whether host drawing is on or off. It also counts delivered vblanks,
 * because a title that paces on vblank cannot present faster than they arrive.
 *
 * RECOMP_FPS=<seconds> prints one line per window to stderr:
 *
 *   [FPS] t=  20.0s  14.9 fps (149 swaps)  vblank  40.0 Hz | run mean 15.1 fps
 *
 * The window is measured from QueryPerformanceCounter, not from the number of
 * seconds asked for, so a reporter thread that wakes late does not read as a
 * slow frame. The run mean is total swaps over the time since the first swap:
 * frame rate swings with what is on screen, so a single window is noise and
 * the mean over the whole run is what two runs should be compared on.
 *
 * Off unless RECOMP_FPS is set; on, it costs one interlocked increment per
 * swap and one per vblank.
 */

#ifdef _WIN32
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

#include "xbox_memory_layout.h"

static volatile LONG64 s_swaps;
static volatile LONG64 s_vblanks;
static volatile LONG   s_started;

static DWORD WINAPI fps_reporter(LPVOID param)
{
    double secs = *(double *)param;
    LARGE_INTEGER qpf, t0, prev, now;
    LONG64 prev_swaps, prev_vblanks, base_vblanks;
    HANDLE timer;

    free(param);
    QueryPerformanceFrequency(&qpf);
    QueryPerformanceCounter(&t0);
    prev = t0;
    prev_swaps = InterlockedCompareExchange64(&s_swaps, 0, 0);
    /* Vblanks start before the first swap. Counting the ones delivered
     * during the boot against the time since the first swap read as a pump
     * running at 62-67 Hz in the first windows' run mean, when every window
     * measured 60.0; the run mean counts from here too. */
    prev_vblanks = base_vblanks = InterlockedCompareExchange64(&s_vblanks, 0, 0);

    /* Sleep is only a wake-up call here; the window is measured, not assumed. */
    timer = CreateWaitableTimerW(NULL, TRUE, NULL);
    for (;;) {
        LONG64 swaps, vblanks;
        double window, elapsed;

        if (timer) {
            LARGE_INTEGER due;
            due.QuadPart = -(LONGLONG)(secs * 10000000.0);
            SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE);
            WaitForSingleObject(timer, INFINITE);
        } else {
            Sleep((DWORD)(secs * 1000.0));
        }

        QueryPerformanceCounter(&now);
        swaps   = InterlockedCompareExchange64(&s_swaps, 0, 0);
        vblanks = InterlockedCompareExchange64(&s_vblanks, 0, 0);
        window  = (double)(now.QuadPart - prev.QuadPart) / (double)qpf.QuadPart;
        elapsed = (double)(now.QuadPart - t0.QuadPart) / (double)qpf.QuadPart;
        if (window <= 0.0)
            window = secs;

        fprintf(stderr, "[FPS] t=%7.1fs %6.1f fps (%lld swaps)  vblank %6.1f Hz"
                " | run mean %.2f fps, vblank %.2f Hz over %.0fs\n",
                elapsed,
                (double)(swaps - prev_swaps) / window, swaps - prev_swaps,
                (double)(vblanks - prev_vblanks) / window,
                (double)swaps / elapsed,
                (double)(vblanks - base_vblanks) / elapsed, elapsed);
        fflush(stderr);
        prev = now;
        prev_swaps = swaps;
        prev_vblanks = vblanks;
    }
    return 0;
}

static void fps_start_once(void)
{
    const char *v;
    double *secs;
    HANDLE h;

    if (InterlockedCompareExchange(&s_started, 1, 0) != 0)
        return;
    v = getenv("RECOMP_FPS");
    if (!v || !*v)
        return;
    secs = (double *)malloc(sizeof *secs);
    if (!secs)
        return;
    *secs = atof(v);
    if (*secs <= 0.0)
        *secs = 10.0;
    h = CreateThread(NULL, 0, fps_reporter, secs, 0, NULL);
    if (h)
        CloseHandle(h);
    else
        free(secs);
}

/* The title presented a frame. The reporter starts at the first one, so the
 * first window is not padded with the boot. */
void xbox_FpsCountSwap(void)
{
    if (!s_started)
        fps_start_once();
    InterlockedIncrement64(&s_swaps);
}

/* The kernel delivered a vblank to the title's ISR. */
void xbox_FpsCountVblank(void)
{
    InterlockedIncrement64(&s_vblanks);
}

#else  /* !_WIN32 */

void xbox_FpsCountSwap(void)   {}
void xbox_FpsCountVblank(void) {}

#endif
