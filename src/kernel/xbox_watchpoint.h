/**
 * Guest memory watchpoints: "what wrote this address?"
 *
 * See xbox_watchpoint.c for why this exists and how to use it.
 */
#ifndef XBOX_WATCHPOINT_H
#define XBOX_WATCHPOINT_H

#include <stdint.h>

#include "platform/recomp_fault.h"

#ifdef _WIN32
#include <windows.h>
#endif

/* Arm whatever RECOMP_WATCH_WRITE asks for. Call once, after guest memory is
 * mapped and the XBE is loaded -- the pages have to exist to be protected. */
void xbox_watch_init(void);

/* Offer a fault to the watchpoints (recomp_fault.h; the runtime's route does
 * this first). Returns non-zero when it was an access to a watched page and
 * has been serviced: resume, and do not report a crash.
 *
 * Windows opens the page and single-steps the instruction with the trap
 * flag, finishing in xbox_watch_handle_trap. POSIX completes the access
 * itself through the guest arena's backdoor, so the page never opens and
 * there is no step. */
int xbox_watch_handle_fault(recomp_fault *f);

/* Offer a single-step trap to the watchpoints. Same return convention. Must
 * be called before any other handling of a single-step. */
int xbox_watch_handle_trap(recomp_fault *f);

#ifdef _WIN32
/* The same two, as the title's VEH used to call them. */
int xbox_watch_handle_av(PEXCEPTION_POINTERS ep, uintptr_t fault_addr,
                         int is_write);
int xbox_watch_handle_step(PEXCEPTION_POINTERS ep);
#else
/* No EXCEPTION_POINTERS here; use the recomp_fault forms above. */
#define xbox_watch_handle_av(ep, addr, wr)      (0)
#define xbox_watch_handle_step(ep)              (0)
#endif

/* RECOMP_WATCH_ARM_ON=<text>: hold the watches until the title opens a file
 * whose path contains <text>. Called with every translated guest path. */
void xbox_watch_note_path(const char *xbox_path);

/* Print every guest address currently holding `value`. Answers "who is
 * holding this bad pointer?", which is the question that gives the
 * watchpoint above an address to watch. Safe to call from a fault handler. */
void xbox_watch_scan_value(uint32_t value);

/* Called from the crash path: runs the scan if RECOMP_FIND_VALUE is set. */
void xbox_watch_scan_on_crash(void);

#endif /* XBOX_WATCHPOINT_H */
