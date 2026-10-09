/*
 * hle_xapi_fiber.c -- XAPI fibers on host fibers.
 *
 * XAPI's SwitchToFiber is five pushes, a store of esp into the old fiber, a
 * load of esp from the new one, five pops and a `ret`: it changes stacks under
 * the caller and returns into whatever the new stack holds. Lifted C cannot
 * follow that -- its frames live on the host stack, not the guest one -- so a
 * switch here returned into the wrong C frame, and a new fiber's first switch
 * reached XapiFiberStartup with someone else's frame and called its start
 * routine through null. Forza Motorsport runs its front end's profile and
 * save work on a fiber; the frame after "New Profile" never came.
 *
 * So each guest fiber gets a host fiber. CreateFiber, ConvertThreadToFiber
 * and DeleteFiber still run the title's own XAPI bodies, so the fiber objects,
 * their guest stacks and GetFiberData stay exactly the title's. SwitchToFiber
 * is replaced: it saves the guest registers the switch preserves (the
 * callee-saved ones, esp and fs:[0], plus the rest, which cost nothing),
 * records the new current fiber where XAPI keeps it, and switches host
 * fibers. A fiber that has not run yet starts on its own guest stack with its
 * start routine called as XapiFiberStartup calls it: start(GetFiberData()).
 * MKDA's task scheduler (titles/mkda/src/mk_tasks.c) does the same for its
 * own coroutines; this is the XAPI form, which any title can use.
 *
 * XAPI keeps the current fiber in its per-thread data, a __declspec(thread)
 * block: [[fs:4] + _tls_index * 4] + 0xC (XapiCurrentFiber_OFFSET). The
 * block's address is taken from ConvertThreadToFiber, whose result is the
 * block's own +0x10, so it is found without a per-title address.
 *
 * Windows only for now: a POSIX host needs ucontext or similar.
 */
#include "hle.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef void (*recomp_func_t)(void);
recomp_func_t recomp_lookup(uint32_t xbox_va);
recomp_func_t recomp_lookup_manual(uint32_t xbox_va);

/* The guest's registers, per host thread (xbox_memory_layout.c). */
extern RECOMP_TLS uint32_t g_eax, g_ecx, g_edx, g_esp, g_ebx, g_esi, g_edi;
extern RECOMP_TLS uint32_t g_ebp, g_seh_ebp;
extern RECOMP_TLS int g_df, g_fp_top;

#define XAPI_CURRENT_FIBER  0x0Cu     /* XapiCurrentFiber_OFFSET */
#define XAPI_THREAD_FIBER   0x10u     /* XapiThreadFiberData_OFFSET */
#define FIBER_MAX           256

typedef struct {
    uint32_t eax, ebx, ecx, edx, esi, edi, esp, ebp, seh_ebp, fs0;
    int      df, fp_top;
} guest_regs;

typedef struct {
    uint32_t   guest;       /* the XAPI fiber object */
    void      *host;        /* NULL until it first runs */
    uint32_t   start;       /* CreateFiber's lpStartAddress */
    guest_regs regs;        /* while switched out */
} guest_fiber;

static guest_fiber      s_fibers[FIBER_MAX];
static int              s_count;
static SRWLOCK          s_lock = SRWLOCK_INIT;

static int fiber_log(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_FIBER_LOG");
        on = e && *e && strcmp(e, "0") != 0;
    }
    return on;
}

/* XAPI's per-thread data on this thread, as ConvertThreadToFiber found it:
 * the fiber it returns is the block's own +0x10 (XapiThreadFiberData_OFFSET),
 * so the block is that less 0x10. A thread must convert before it switches,
 * so a thread that never did has none. */
static RECOMP_TLS uint32_t t_thread_data;

static uint32_t xapi_thread_data(void)
{
    return t_thread_data;
}

static guest_fiber *fiber_find(uint32_t guest)
{
    int i;
    for (i = 0; i < s_count; i++)
        if (s_fibers[i].guest == guest)
            return &s_fibers[i];
    return NULL;
}

/* An entry for this guest fiber, reusing a deleted slot. */
static guest_fiber *fiber_add(uint32_t guest)
{
    guest_fiber *f;
    int i;

    AcquireSRWLockExclusive(&s_lock);
    f = fiber_find(guest);
    if (!f) {
        for (i = 0; i < s_count && s_fibers[i].guest; i++)
            ;
        if (i == s_count && s_count < FIBER_MAX)
            s_count++;
        if (i < s_count) {
            f = &s_fibers[i];
            memset(f, 0, sizeof *f);
            f->guest = guest;
        }
    }
    ReleaseSRWLockExclusive(&s_lock);
    if (!f) {
        fprintf(stderr, "[FIBER] more than %d guest fibers at once\n", FIBER_MAX);
        fflush(stderr);
    }
    return f;
}

static void regs_save(guest_regs *r)
{
    r->eax = g_eax; r->ebx = g_ebx; r->ecx = g_ecx; r->edx = g_edx;
    r->esi = g_esi; r->edi = g_edi; r->esp = g_esp; r->ebp = g_ebp;
    r->seh_ebp = g_seh_ebp; r->fs0 = HLE_MEM32(XBOX_FS_BASE);
    r->df = g_df; r->fp_top = g_fp_top;
}

static void regs_load(const guest_regs *r)
{
    g_eax = r->eax; g_ebx = r->ebx; g_ecx = r->ecx; g_edx = r->edx;
    g_esi = r->esi; g_edi = r->edi; g_esp = r->esp; g_ebp = r->ebp;
    g_seh_ebp = r->seh_ebp; HLE_MEM32(XBOX_FS_BASE) = r->fs0;
    g_df = r->df; g_fp_top = r->fp_top;
}

/* A fiber's first run: XapiFiberStartup's work, on the fiber's own stack. */
static void WINAPI fiber_main(void *p)
{
    guest_fiber *f = (guest_fiber *)p;
    recomp_func_t fn = recomp_lookup_manual(f->start);
    uint32_t param = HLE_MEM32(f->guest);      /* GetFiberData() */

    if (!fn)
        fn = recomp_lookup(f->start);
    /* The object sits at the top of the stack CreateFiber allocated, which
     * grows down from below it. */
    g_esp = (f->guest - 0x20u) & ~0xFu;
    g_ebp = g_seh_ebp = 0;
    HLE_MEM32(XBOX_FS_BASE) = 0xFFFFFFFFu;     /* no SEH frames yet */
    g_df = 0;
    if (fiber_log())
        fprintf(stderr, "[FIBER] 0x%08X starts at 0x%08X with 0x%08X\n",
                f->guest, f->start, param);
    if (fn) {
        g_esp -= 4; HLE_MEM32(g_esp) = param;
        g_esp -= 4; HLE_MEM32(g_esp) = 0;      /* XapiFiberStartup never returns */
        fn();
    }
    /* XapiFiberStartup ends the thread when the routine returns. */
    fprintf(stderr, "[FIBER] 0x%08X's start routine 0x%08X %s; the thread ends, "
            "as XapiFiberStartup ends it\n", f->guest, f->start,
            fn ? "returned" : "is not a lifted function");
    fflush(stderr);
    ExitThread(0);
}

HLE_ORIGINAL(CreateFiber);
HLE_ORIGINAL(ConvertThreadToFiber);
HLE_ORIGINAL(DeleteFiber);

/* LPVOID CreateFiber(DWORD dwStackSize, LPFIBER_START_ROUTINE lpStartAddress,
 *     LPVOID lpParameter)                                                     */
HLE_EXPORT(CreateFiber)
{
    guest_fiber *f;

    if (!hle_original_CreateFiber)
        HLE_RETURN(0u);
    HLE_CALL_ORIGINAL(CreateFiber);
    if (g_eax && (f = fiber_add(g_eax)) != NULL)
        f->start = HLE_ARG(1);
    if (fiber_log())
        fprintf(stderr, "[FIBER] CreateFiber(0x%X, 0x%08X, 0x%08X) -> 0x%08X, stack 0x%08X-0x%08X\n",
                HLE_ARG(0), HLE_ARG(1), HLE_ARG(2), g_eax,
                g_eax ? HLE_MEM32(g_eax + 8) : 0u, g_eax ? HLE_MEM32(g_eax + 4) : 0u);
}

/* LPVOID ConvertThreadToFiber(LPVOID lpParameter)                            */
HLE_EXPORT(ConvertThreadToFiber)
{
    guest_fiber *f;

    if (!hle_original_ConvertThreadToFiber)
        HLE_RETURN(0u);
    HLE_CALL_ORIGINAL(ConvertThreadToFiber);
    if (g_eax)
        t_thread_data = g_eax - XAPI_THREAD_FIBER;
    if (g_eax && (f = fiber_add(g_eax)) != NULL) {
        f->host = IsThreadAFiber() ? GetCurrentFiber() : ConvertThreadToFiber(NULL);
        if (fiber_log())
            fprintf(stderr, "[FIBER] thread %lu is fiber 0x%08X\n",
                    GetCurrentThreadId(), g_eax);
    }
}

/* void DeleteFiber(LPVOID lpFiber)                                           */
HLE_EXPORT(DeleteFiber)
{
    uint32_t guest = HLE_ARG(0);
    guest_fiber *f;

    AcquireSRWLockExclusive(&s_lock);
    f = fiber_find(guest);
    if (f) {
        /* Never the running one: XAPI's DeleteFiber of the current fiber
         * ends the thread, and that path is not taken here. */
        if (f->host && f->host != GetCurrentFiber() && f->start)
            DeleteFiber(f->host);
        f->guest = 0;
        f->host = NULL;
    }
    ReleaseSRWLockExclusive(&s_lock);
    if (hle_original_DeleteFiber)
        HLE_CALL_ORIGINAL(DeleteFiber);
}

/* void SwitchToFiber(LPVOID lpFiber)                                         */
HLE_EXPORT(SwitchToFiber)
{
    uint32_t target = HLE_ARG(0), data = xapi_thread_data();
    uint32_t current = data ? HLE_MEM32(data + XAPI_CURRENT_FIBER) : 0u;
    guest_fiber *from, *to;

    if (!data || target == current)
        return;
    to = fiber_find(target);
    from = fiber_find(current);
    if (!from && current) {
        /* A thread XAPI made a fiber before this file saw it. */
        from = fiber_add(current);
        if (from)
            from->host = IsThreadAFiber() ? GetCurrentFiber() : ConvertThreadToFiber(NULL);
    }
    if (!to || !from || !from->host) {
        static int said;
        if (said++ < 8) {
            fprintf(stderr, "[FIBER] SwitchToFiber(0x%08X) from 0x%08X: %s; not switched\n",
                    target, current, !to ? "not a fiber CreateFiber made" : "no host fiber here");
            fflush(stderr);
        }
        return;
    }
    if (!to->host) {
        to->host = CreateFiberEx(64 * 1024, 2 * 1024 * 1024, 0, fiber_main, to);
        if (!to->host) {
            fprintf(stderr, "[FIBER] CreateFiberEx failed (%lu)\n", GetLastError());
            fflush(stderr);
            return;
        }
    }
    if (fiber_log())
        fprintf(stderr, "[FIBER] switch 0x%08X -> 0x%08X\n", current, target);
    regs_save(&from->regs);
    HLE_MEM32(data + XAPI_CURRENT_FIBER) = target;
    SwitchToFiber(to->host);
    /* Back on this fiber: whoever switched here already made it current. */
    regs_load(&from->regs);
}

#else /* !_WIN32 */

/* No host fibers here yet: the title's own bodies, as before this file. The
 * markers are scanned whatever the platform, so the names must exist. */
HLE_ORIGINAL(CreateFiber);
HLE_ORIGINAL(ConvertThreadToFiber);
HLE_ORIGINAL(DeleteFiber);
HLE_ORIGINAL(SwitchToFiber);
HLE_EXPORT(CreateFiber)          { if (hle_original_CreateFiber) HLE_CALL_ORIGINAL(CreateFiber); }
HLE_EXPORT(ConvertThreadToFiber) { if (hle_original_ConvertThreadToFiber) HLE_CALL_ORIGINAL(ConvertThreadToFiber); }
HLE_EXPORT(DeleteFiber)          { if (hle_original_DeleteFiber) HLE_CALL_ORIGINAL(DeleteFiber); }
HLE_EXPORT(SwitchToFiber)        { if (hle_original_SwitchToFiber) HLE_CALL_ORIGINAL(SwitchToFiber); }

#endif /* _WIN32 */
