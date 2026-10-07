/*
 * fault_posix.c - recomp_fault.h on POSIX: a signal handler.
 *
 * Deliberately minimal for now (mac-memory-faults owns the full version):
 * it fills a recomp_fault from the signal and the machine context, asks the
 * route, reports what the route declined, and lets the signal's default
 * action end the process. The same order as fault_win32.c.
 *
 * Runs on an alternate signal stack, so a fault on an exhausted stack can
 * still be reported: recomp_fault_thread_init gives each thread one.
 */
#ifndef _WIN32

/* ucontext_t's register names on Linux (REG_RIP, ...). */
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif

#include "recomp_fault.h"
#include "mmio_decode_a64.h"   /* the Linux arm64 ESR record */

#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifdef __APPLE__
#include <sys/ucontext.h>   /* <ucontext.h> wants _XOPEN_SOURCE for routines this does not use */
#else
#include <ucontext.h>
#endif

static const int g_signals[] = { SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGTRAP };

static int kind_of(int sig, const siginfo_t *si)
{
    switch (sig) {
    case SIGSEGV:
    case SIGBUS:
        return RECOMP_FAULT_ACCESS;
    case SIGILL:
        return RECOMP_FAULT_ILLEGAL;
    case SIGFPE:
        if (si->si_code == FPE_INTDIV) return RECOMP_FAULT_INT_DIVIDE;
        if (si->si_code == FPE_INTOVF) return RECOMP_FAULT_INT_OVERFLOW;
        return RECOMP_FAULT_OTHER;
    case SIGTRAP:
        return si->si_code == TRAP_TRACE ? RECOMP_FAULT_SINGLE_STEP
                                         : RECOMP_FAULT_BREAKPOINT;
    default:
        return RECOMP_FAULT_OTHER;
    }
}

/* pc, sp and -- where the hardware says -- whether a data access was a
 * write, from the machine context. */
static void read_context(recomp_fault *f, ucontext_t *uc)
{
#if defined(__APPLE__) && defined(__aarch64__)
    uint32_t esr = uc->uc_mcontext->__es.__esr;
    uint32_t ec = esr >> 26;

    f->pc = (uintptr_t)uc->uc_mcontext->__ss.__pc;
    f->sp = (uintptr_t)uc->uc_mcontext->__ss.__sp;
    /* WnR (bit 6) means something only for a data abort (EC 0x24 from a
     * lower exception level, 0x25 from the same one). */
    if (f->kind == RECOMP_FAULT_ACCESS && (ec == 0x24 || ec == 0x25))
        f->is_write = (esr >> 6) & 1;
#elif defined(__APPLE__) && defined(__x86_64__)
    f->pc = (uintptr_t)uc->uc_mcontext->__ss.__rip;
    f->sp = (uintptr_t)uc->uc_mcontext->__ss.__rsp;
    if (f->kind == RECOMP_FAULT_ACCESS)
        f->is_write = (uc->uc_mcontext->__es.__err >> 1) & 1;
#elif defined(__linux__) && defined(__x86_64__)
    f->pc = (uintptr_t)uc->uc_mcontext.gregs[REG_RIP];
    f->sp = (uintptr_t)uc->uc_mcontext.gregs[REG_RSP];
    if (f->kind == RECOMP_FAULT_ACCESS)
        f->is_write = (int)((uc->uc_mcontext.gregs[REG_ERR] >> 1) & 1);
#elif defined(__linux__) && defined(__aarch64__)
    f->pc = (uintptr_t)uc->uc_mcontext.pc;
    f->sp = (uintptr_t)uc->uc_mcontext.sp;
    /* The ESR is an esr_context record in __reserved. */
    if (f->kind == RECOMP_FAULT_ACCESS)
        f->is_write = mmio_a64_esr_is_write(mmio_a64_esr(uc));
#else
    (void)uc;
#endif
}

/* Set while this thread is inside the handler. A fault in the route or the
 * report itself -- a different signal, since the one being handled is
 * blocked -- must not go round again: it would re-enter the code that just
 * faulted, on the same alternate stack. */
static __thread volatile sig_atomic_t t_in_handler;

static void on_signal(int sig, siginfo_t *si, void *ucv)
{
    recomp_fault_route_fn route = recomp_fault_route_cb();
    recomp_crash_fn crash = recomp_fault_crash_cb();
    recomp_fault f;

    if (t_in_handler) {
        static const char msg[] = "[CRASH] fault inside the fault handler\n";
        ssize_t w = write(2, msg, sizeof msg - 1);
        (void)w;
        signal(sig, SIG_DFL);
        return;                         /* re-faults with the default action */
    }
    t_in_handler = 1;

    memset(&f, 0, sizeof f);
    f.kind      = kind_of(sig, si);
    f.code      = (uint32_t)sig;
    f.host_addr = (uintptr_t)si->si_addr;
    f.is_write  = -1;
    f.native    = si;
    f.ctx       = ucv;
    read_context(&f, (ucontext_t *)ucv);

    if (route && route(&f)) {
        t_in_handler = 0;
        return;                         /* resume where ctx now says */
    }

    if (f.kind != RECOMP_FAULT_BREAKPOINT && crash)
        crash(&f);
    fflush(stdout);
    fflush(stderr);

    /* The default action, as if no handler were installed: it is delivered
     * as this handler returns (the signal is blocked while it runs). */
    signal(sig, SIG_DFL);
    raise(sig);
    t_in_handler = 0;
}

/* The alternate stack goes when its thread does. */
static pthread_key_t  g_altstack_key;
static pthread_once_t g_altstack_once = PTHREAD_ONCE_INIT;

static void altstack_free(void *mem)
{
    stack_t ss;

    memset(&ss, 0, sizeof ss);
    ss.ss_flags = SS_DISABLE;
    sigaltstack(&ss, NULL);
    free(mem);
}

static void altstack_key_init(void)
{
    pthread_key_create(&g_altstack_key, altstack_free);
}

void recomp_fault_thread_init(void)
{
    stack_t ss;

    pthread_once(&g_altstack_once, altstack_key_init);
    if (pthread_getspecific(g_altstack_key))
        return;
    memset(&ss, 0, sizeof ss);
    /* Enough for the report, which prints and symbolises but does not
     * recurse. */
    ss.ss_size = 256 * 1024;
    ss.ss_sp = malloc(ss.ss_size);
    if (!ss.ss_sp)
        return;
    if (sigaltstack(&ss, NULL) != 0) {
        free(ss.ss_sp);
        return;
    }
    pthread_setspecific(g_altstack_key, ss.ss_sp);
}

void recomp_fault_install(recomp_fault_route_fn route, recomp_crash_fn crash)
{
    struct sigaction sa;
    size_t i;

    recomp_fault_set_cbs(route, crash);
    recomp_fault_thread_init();
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_signal;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    for (i = 0; i < sizeof g_signals / sizeof g_signals[0]; i++)
        sigaction(g_signals[i], &sa, NULL);
}

#endif /* !_WIN32 */
