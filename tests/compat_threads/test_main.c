/*
 * compat_threads -- the POSIX Win32 shims a title's threads lean on:
 * fibers (MKDA's mk_tasks.c), GetCurrentThreadStackLimits (inside and
 * outside a fiber), and SuspendThread/ResumeThread on a running thread.
 *
 * POSIX only: on Windows these are the real kernel32 functions.
 */
#include "win32_compat.h"

#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", \
                      __FILE__, __LINE__, #c); failures++; } } while (0)

/* ---- fibers ------------------------------------------------------------ */

static LPVOID g_main, g_a, g_b;
static int g_trace[32], g_ntrace;
static uintptr_t g_a_lo, g_a_hi, g_a_local;
static jmp_buf g_a_root;

static int recurse(int n)       /* uses some stack, defeats tail calls */
{
    volatile char pad[256];
    pad[0] = 1;
    return n ? recurse(n - 1) + pad[0] : 0;
}

static void WINAPI fiber_a(LPVOID p)
{
    volatile double keep = 1.25;        /* lives in a callee-saved FP reg */
    ULONG_PTR lo, hi;
    volatile int here = 0;

    CHECK(p == (LPVOID)0xA);
    CHECK(GetFiberData() == (LPVOID)0xA);
    CHECK(GetCurrentFiber() == g_a);
    GetCurrentThreadStackLimits(&lo, &hi);
    g_a_lo = lo; g_a_hi = hi; g_a_local = (uintptr_t)&here;
    CHECK((uintptr_t)&here >= lo && (uintptr_t)&here < hi);
    CHECK(recurse(1000) > 0);

    g_trace[g_ntrace++] = 1;
    SwitchToFiber(g_b);
    g_trace[g_ntrace++] = 3;
    CHECK(keep == 1.25);
    if (setjmp(g_a_root) == 0) {
        SwitchToFiber(g_main);      /* main switches back; then we longjmp */
        longjmp(g_a_root, 7);
    }
    g_trace[g_ntrace++] = 5;
    SwitchToFiber(g_main);
    CHECK(!"fiber a resumed after its last switch");
}

static void WINAPI fiber_b(LPVOID p)
{
    (void)p;
    g_trace[g_ntrace++] = 2;
    SwitchToFiber(g_a);
    CHECK(!"fiber b resumed");
}

static void test_fibers(void)
{
    ULONG_PTR lo, hi;
    volatile int here = 0;
    volatile double keep = 3.5;

    CHECK(!IsThreadAFiber());
    CHECK(GetCurrentFiber() == NULL);
    GetCurrentThreadStackLimits(&lo, &hi);
    CHECK(lo < hi);
    CHECK((uintptr_t)&here >= lo && (uintptr_t)&here < hi);

    g_main = ConvertThreadToFiber(NULL);
    CHECK(g_main != NULL);
    CHECK(IsThreadAFiber());
    CHECK(ConvertThreadToFiber(NULL) == NULL);
    CHECK(GetLastError() == ERROR_ALREADY_FIBER);
    {
        ULONG_PTR lo2, hi2;
        GetCurrentThreadStackLimits(&lo2, &hi2);   /* the thread's own */
        CHECK(lo2 == lo && hi2 == hi);
    }

    g_a = CreateFiberEx(64 * 1024, 2 * 1024 * 1024, 0, fiber_a, (LPVOID)0xA);
    g_b = CreateFiber(0, fiber_b, (LPVOID)0xB);
    CHECK(g_a && g_b);

    SwitchToFiber(g_a);             /* a -> b -> a -> main */
    CHECK(GetCurrentFiber() == g_main);
    CHECK(keep == 3.5);
    CHECK(g_ntrace == 3 && g_trace[0] == 1 && g_trace[1] == 2 && g_trace[2] == 3);
    CHECK(g_a_hi - g_a_lo >= 2 * 1024 * 1024);
    CHECK(!((uintptr_t)&here >= g_a_lo && (uintptr_t)&here < g_a_hi));
    CHECK(g_a_local >= g_a_lo && g_a_local < g_a_hi);

    SwitchToFiber(g_a);             /* longjmp inside a, then back */
    CHECK(g_ntrace == 4 && g_trace[3] == 5);

    DeleteFiber(g_a);
    DeleteFiber(g_b);
}

/* ---- suspend / resume ---------------------------------------------------- */

static volatile LONG g_count;
static volatile LONG g_stop;

static DWORD WINAPI spinner(LPVOID p)
{
    (void)p;
    while (!g_stop)
        InterlockedIncrement(&g_count);
    return 0;
}

static int moves(void)
{
    LONG a = g_count;
    Sleep(30);
    return g_count != a;
}

static volatile LONG g_self_phase;
static DWORD WINAPI self_suspender(LPVOID p)
{
    (void)p;
    g_self_phase = 1;
    SuspendThread(GetCurrentThread());
    g_self_phase = 2;
    return 0;
}

static void test_suspend(void)
{
    HANDLE h;
    DWORD prev;

    /* CREATE_SUSPENDED: nothing runs until the count reaches 0. */
    g_count = 0;
    h = CreateThread(NULL, 0, spinner, NULL, CREATE_SUSPENDED, NULL);
    CHECK(h != NULL);
    Sleep(30);
    CHECK(g_count == 0);
    CHECK(ResumeThread(h) == 1);
    Sleep(30);
    CHECK(moves());

#if defined(__APPLE__)
    /* A running thread stops on the 0 -> 1 edge and only runs again at 0. */
    prev = SuspendThread(h);
    CHECK(prev == 0);
    Sleep(5);
    CHECK(!moves());
    CHECK(SuspendThread(h) == 1);
    CHECK(ResumeThread(h) == 2);
    CHECK(!moves());
    CHECK(ResumeThread(h) == 1);
    CHECK(moves());
    CHECK(ResumeThread(h) == 0);    /* not suspended: no-op */
    CHECK(moves());
#else
    (void)prev;
#endif
    g_stop = 1;
    CHECK(WaitForSingleObject(h, 2000) == WAIT_OBJECT_0);
    CloseHandle(h);

    /* A thread suspending itself waits for someone else's resume. */
    h = CreateThread(NULL, 0, self_suspender, NULL, 0, NULL);
    while (g_self_phase == 0)
        Sleep(1);
    Sleep(30);
    CHECK(g_self_phase == 1);
    CHECK(ResumeThread(h) == 1);
    CHECK(WaitForSingleObject(h, 2000) == WAIT_OBJECT_0);
    CHECK(g_self_phase == 2);
    CloseHandle(h);
}

/* Fibers on a second thread: t_fiber is per thread. */
static LPVOID g_t_main, g_t_f;
static volatile int g_t_ran;
static void WINAPI t_fiber(LPVOID p)
{
    (void)p;
    g_t_ran = 1;
    SwitchToFiber(g_t_main);
}
static DWORD WINAPI fiber_thread(LPVOID p)
{
    (void)p;
    CHECK(!IsThreadAFiber());
    g_t_main = ConvertThreadToFiber(NULL);
    g_t_f = CreateFiber(0, t_fiber, NULL);
    SwitchToFiber(g_t_f);
    CHECK(g_t_ran == 1);
    DeleteFiber(g_t_f);
    return 0;
}

/* Stack sizes as Windows gives them: a size is a commit, the reservation is
 * at least the default, unless STACK_SIZE_PARAM_IS_A_RESERVATION. */
static ULONG_PTR g_stack_bytes;
static DWORD WINAPI stack_probe(LPVOID p)
{
    ULONG_PTR lo, hi;
    (void)p;
    GetCurrentThreadStackLimits(&lo, &hi);
    g_stack_bytes = hi - lo;
    return 0;
}

static ULONG_PTR stack_of(SIZE_T size, DWORD flags)
{
    HANDLE h;
    /* Before the thread exists: reset after CreateThread, a probe that had
     * already run would have its answer wiped (a flaky failure). */
    g_stack_bytes = 0;
    h = CreateThread(NULL, size, stack_probe, NULL, flags, NULL);
    CHECK(h != NULL);
    CHECK(WaitForSingleObject(h, 2000) == WAIT_OBJECT_0);
    CloseHandle(h);
    return g_stack_bytes;
}

static void test_stack_sizes(void)
{
    CHECK(stack_of(0, 0) >= (8u << 20));
    CHECK(stack_of(16 * 1024, 0) >= (8u << 20));
    CHECK(stack_of(32u << 20, 0) >= (32u << 20));
    {
        ULONG_PTR r = stack_of(256 * 1024, STACK_SIZE_PARAM_IS_A_RESERVATION);
        CHECK(r >= 256 * 1024 && r < (1u << 20));
    }
}

int main(void)
{
    HANDLE h;

    test_fibers();
    test_suspend();
    test_stack_sizes();
    h = CreateThread(NULL, 0, fiber_thread, NULL, 0, NULL);
    CHECK(WaitForSingleObject(h, 2000) == WAIT_OBJECT_0);
    CloseHandle(h);

    if (failures) {
        fprintf(stderr, "compat_threads: %d failure(s)\n", failures);
        return 1;
    }
    printf("compat_threads: all passed\n");
    return 0;
}
