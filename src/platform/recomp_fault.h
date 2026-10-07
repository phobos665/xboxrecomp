/*
 * recomp_fault.h - host faults, the same shape on every host.
 *
 * Lifted code faults as native code: a guest access to a trapped device page,
 * an armed watchpoint, or a real crash all arrive as a host exception -- a
 * vectored exception on Windows (fault_win32.c), a signal on POSIX
 * (fault_posix.c). Both build a recomp_fault and hand it to two callbacks:
 *
 *   route  "is this one mine?" -- watchpoints and trapped device registers,
 *          which are serviced and resumed. Returns 1 when handled.
 *   crash  the report, for everything route declined. The process then dies
 *          the way it would have without a handler.
 *
 * This library is the bottom of the stack (the kernel, the APU and the rest
 * link it), so the callbacks are passed in rather than named: the route lives
 * in the runtime, the report in the title's main.c.
 *
 * No <windows.h> here: `native` and `ctx` carry the host's own records for
 * the decoders that need them.
 */
#ifndef RECOMP_FAULT_H
#define RECOMP_FAULT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    RECOMP_FAULT_ACCESS = 1,     /* a data access to memory that refused it */
    RECOMP_FAULT_SINGLE_STEP,    /* the trap flag, which watchpoints set */
    RECOMP_FAULT_BREAKPOINT,     /* int 3 / brk */
    RECOMP_FAULT_ILLEGAL,        /* an undefined or privileged instruction */
    RECOMP_FAULT_INT_DIVIDE,     /* integer divide by zero */
    RECOMP_FAULT_INT_OVERFLOW,   /* integer divide overflow (INT_MIN / -1) */
    RECOMP_FAULT_OTHER           /* anything else: see `code` */
};

typedef struct recomp_fault {
    uintptr_t host_addr;  /* the faulting data address (Windows:
                           * ExceptionInformation[1]; POSIX: si_addr, untagged) */
    int       is_write;   /* 1 write, 0 read, -1 not known: decode the instruction */
    int       kind;       /* RECOMP_FAULT_* */
    uint32_t  code;       /* the host's own code: an NTSTATUS on Windows, the
                           * signal number on POSIX */
    uintptr_t pc, sp;     /* host program counter and stack pointer */
    void     *native;     /* PEXCEPTION_POINTERS (Windows) / siginfo_t * (POSIX);
                           * NULL for recomp_fault_raise */
    void     *ctx;        /* PCONTEXT (Windows) / ucontext_t * (POSIX): the
                           * decoders write the resumed registers here.
                           * NULL for recomp_fault_raise */
} recomp_fault;

/* 1: handled, resume where `ctx` now says. 0: not mine. */
typedef int  (*recomp_fault_route_fn)(recomp_fault *f);
/* Report a fault nobody handled. Must not assume it can allocate. */
typedef void (*recomp_crash_fn)(const recomp_fault *f);

/* Install the host handler, first in line, for the whole process. Either
 * callback may be NULL. On Windows this also loads the symbols
 * host_symbol_name uses, up front: at fault time the process is already in
 * a bad way, and SymInitialize allocates. Call once, early, on the thread
 * that will run the guest. */
void recomp_fault_install(recomp_fault_route_fn route, recomp_crash_fn crash);

/* Per-thread set-up: on POSIX the alternate signal stack a fault on an
 * exhausted stack needs. Nothing on Windows. Every thread that runs guest
 * code calls it first (win32_compat's CreateThread and host_main_posix do). */
void recomp_fault_thread_init(void);

/* A fault the host does not trap, raised by the runtime itself: an integer
 * divide on AArch64, which returns 0 rather than faulting. Builds a fault
 * (native and ctx NULL, sp this frame's), routes it, reports it, and exits
 * as an unhandled exception would, with `code` as the status. Returns only
 * when route handled it. */
void recomp_fault_raise(int kind, uint32_t code, uintptr_t pc);

/* The callbacks recomp_fault_install was given, for the host halves. */
recomp_fault_route_fn recomp_fault_route_cb(void);
recomp_crash_fn       recomp_fault_crash_cb(void);
void                  recomp_fault_set_cbs(recomp_fault_route_fn route,
                                           recomp_crash_fn crash);

#ifdef __cplusplus
}
#endif

#endif /* RECOMP_FAULT_H */
