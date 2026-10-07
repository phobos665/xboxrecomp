/*
 * fault_common.c - the parts of recomp_fault.h that are the same everywhere:
 * the installed callbacks, and faults the runtime raises itself.
 */
#include "recomp_fault.h"

#include <stdio.h>
#include <stdlib.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

static recomp_fault_route_fn g_route;
static recomp_crash_fn       g_crash;

void recomp_fault_set_cbs(recomp_fault_route_fn route, recomp_crash_fn crash)
{
    g_route = route;
    g_crash = crash;
}

recomp_fault_route_fn recomp_fault_route_cb(void) { return g_route; }
recomp_crash_fn       recomp_fault_crash_cb(void) { return g_crash; }

void recomp_fault_raise(int kind, uint32_t code, uintptr_t pc)
{
    recomp_fault f;
    volatile char here = 0;

    f.host_addr = 0;
    f.is_write  = -1;
    f.kind      = kind;
    f.code      = code;
    f.pc        = pc;
    /* This frame's stack, near enough for a scan of return addresses, and
     * portable (MSVC has no __builtin_frame_address). */
    f.sp        = (uintptr_t)&here;
    f.native    = NULL;
    f.ctx       = NULL;
    if (g_route && g_route(&f))
        return;
    if (g_crash)
        g_crash(&f);
    fflush(stdout);
    fflush(stderr);
    /* As an unhandled exception ends a process: no atexit handlers, which
     * would run guest-facing shutdown on a thread that is mid-fault. */
#ifdef _WIN32
    TerminateProcess(GetCurrentProcess(), code);
#else
    _exit((int)(code & 0xFF) ? (int)(code & 0xFF) : 1);
#endif
}
