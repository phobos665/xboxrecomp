/*
 * fault_emulate.c - see fault_emulate.h.
 *
 * Everything here may run inside a fault handler -- a signal handler on
 * POSIX -- so none of it allocates, takes a lock, or touches memory that can
 * fault. A second fault while the first is being handled is fatal on POSIX
 * (the signal is blocked), which is why guest memory is only ever read here
 * through the backdoor.
 */
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE   /* REG_RIP and friends */
#endif

#include "fault_emulate.h"

#include <stdio.h>
#include <string.h>

#include "mmio_decode.h"
#include "mmio_decode_a64.h"

#if defined(_WIN32)
#include <windows.h>
#else
#include "win32_compat.h"
#endif

static volatile uintptr_t s_guest_base;

void recomp_fault_set_guest_base(uintptr_t base) { s_guest_base = base; }
uintptr_t recomp_fault_guest_base(void) { return s_guest_base; }

/* ---- ranges -------------------------------------------------------------- */

#define MAX_RANGES 16

typedef struct {
    uint32_t        lo, hi;
    int             write_only;
    recomp_range_fn fn;
    const char     *name;
} fault_range;

static fault_range  s_ranges[MAX_RANGES];
static volatile int s_nranges;

int recomp_fault_add_range(uint32_t lo_va, uint32_t hi_va, int write_only,
                           recomp_range_fn fn, const char *name)
{
    int n = s_nranges;

    if (!fn || hi_va <= lo_va || n >= MAX_RANGES)
        return -1;
    for (int i = 0; i < n; i++)
        if (s_ranges[i].lo == lo_va && s_ranges[i].hi == hi_va && s_ranges[i].fn == fn)
            return 0;                          /* registered twice: once is enough */
    s_ranges[n].lo = lo_va;
    s_ranges[n].hi = hi_va;
    s_ranges[n].write_only = write_only;
    s_ranges[n].fn = fn;
    s_ranges[n].name = name;
    __atomic_store_n(&s_nranges, n + 1, __ATOMIC_RELEASE);
    return 0;
}

int recomp_fault_dispatch_ranges(recomp_fault *f, uint32_t va)
{
    int n = __atomic_load_n(&s_nranges, __ATOMIC_ACQUIRE);

    for (int i = 0; i < n; i++) {
        const fault_range *r = &s_ranges[i];
        if (va < r->lo || va >= r->hi)
            continue;
        /* -1 (not known) is offered as a write: on a write-only trap page a
         * read never faults, so an unknown direction there is a write. */
        if (r->write_only && f->is_write == 0)
            continue;
        if (r->fn(f, va))
            return 1;
    }
    return 0;
}

/* ---- the decoders, for this host's CPU ---------------------------------- */

/* Run the instruction at f's pc against rd/wr; commit says whether the
 * registers are written back. 1 when the decoder knew the instruction. */
static int emulate(recomp_fault *f, uintptr_t dev_base, void *dev,
                   mmio_read_fn rd, mmio_write_fn wr, int commit)
{
    if (!f->ctx)
        return 0;                         /* a raised fault has no context */
#if defined(__aarch64__) && defined(MMIO_A64_HAVE_UCONTEXT)
    {
        mmio_a64_ctx c;
        mmio_a64_from_ucontext(&c, (ucontext_t *)f->ctx);
        if (!mmio_a64_emulate(&c, *(const uint32_t *)(uintptr_t)c.pc, dev_base, dev, rd, wr))
            return 0;
        if (commit) {
            mmio_a64_to_ucontext(&c, (ucontext_t *)f->ctx);
            f->pc = (uintptr_t)c.pc;
        }
        return 1;
    }
#elif defined(MMIO_X86_DECODER) && defined(_WIN32)
    {
        mmio_x86_ctx copy, *c = (mmio_x86_ctx *)f->ctx;
        if (!commit) {
            copy = *c;
            c = &copy;
        }
        if (!mmio_emulate(c, (uint32_t)(f->host_addr - dev_base), dev, rd, wr))
            return 0;
        if (commit)
            f->pc = (uintptr_t)c->Rip;
        return 1;
    }
#elif defined(MMIO_X86_DECODER) && defined(MMIO_X86_HAVE_UCONTEXT)
    {
        mmio_x86_ctx c;
        mmio_x86_from_ucontext(&c, (ucontext_t *)f->ctx);
        if (!mmio_emulate(&c, (uint32_t)(f->host_addr - dev_base), dev, rd, wr))
            return 0;
        if (commit) {
            mmio_x86_to_ucontext(&c, (ucontext_t *)f->ctx);
            f->pc = (uintptr_t)c.Rip;
        }
        return 1;
    }
#else
    (void)dev_base; (void)dev; (void)rd; (void)wr; (void)commit;
    return 0;
#endif
}

int recomp_fault_emulate(recomp_fault *f, uintptr_t dev_base, void *dev,
                         mmio_read_fn rd, mmio_write_fn wr)
{
    return emulate(f, dev_base, dev, rd, wr, 1);
}

/* ---- guest memory without faulting --------------------------------------- */

#if !defined(_WIN32)
/* A byte-exact copy between a guest range and a buffer through the backdoor,
 * a 4 KB page at a time, since consecutive guest pages can belong to
 * different placements. 0 if any byte has no backdoor. */
static int backdoor_copy(uint32_t va, void *buf, int size, int to_guest)
{
    uintptr_t base = s_guest_base;
    uint8_t *b = (uint8_t *)buf;

    while (size > 0) {
        uint8_t *bd = (uint8_t *)w32_backdoor((const void *)(base + va));
        int chunk = (int)(0x1000u - (va & 0xFFFu));
        if (chunk > size)
            chunk = size;
        if (!bd)
            return 0;
        if (to_guest) memcpy(bd, b, (size_t)chunk);
        else          memcpy(b, bd, (size_t)chunk);
        va += (uint32_t)chunk;
        b += chunk;
        size -= chunk;
    }
    return 1;
}
#endif

uint64_t recomp_guest_read(void *dev, uint32_t va, int size)
{
    uint64_t v = 0;
    (void)dev;
    if (size > 8) size = 8;
#if defined(_WIN32)
    memcpy(&v, (const void *)(s_guest_base + va), (size_t)size);
#else
    backdoor_copy(va, &v, size, 0);
#endif
    return v;
}

void recomp_guest_write(void *dev, uint32_t va, uint64_t val, int size)
{
    (void)dev;
    if (size > 8) size = 8;
#if defined(_WIN32)
    memcpy((void *)(s_guest_base + va), &val, (size_t)size);
#else
    backdoor_copy(va, &val, size, 1);
#endif
}

/* ---- checked completion through the backdoor ----------------------------- */

#if !defined(_WIN32)
typedef struct {
    recomp_allow_fn allow;
    void           *arg;
    int             refused;
} dry_run;

static __thread dry_run *t_dry;

static uint64_t dry_rd(void *dev, uint32_t va, int size)
{
    dry_run *d = t_dry;
    (void)dev;
    if (!d->allow(s_guest_base + va, size, 0, d->arg))
        d->refused = 1;
    /* The real value, so a compare-and-swap takes the branch it will take
     * for real and its store is checked too. Reading has no side effect. */
    return recomp_guest_read(NULL, va, size);
}

static void dry_wr(void *dev, uint32_t va, uint64_t val, int size)
{
    dry_run *d = t_dry;
    (void)dev; (void)val;
    if (!d->allow(s_guest_base + va, size, 1, d->arg))
        d->refused = 1;
}
#endif

int recomp_fault_complete_guest(recomp_fault *f, recomp_allow_fn allow, void *arg)
{
#if defined(_WIN32)
    (void)f; (void)allow; (void)arg;
    return 0;
#else
    dry_run d;
    uintptr_t base = s_guest_base;

    if (!base || !f->ctx || !allow)
        return 0;
    d.allow = allow;
    d.arg = arg;
    d.refused = 0;
    t_dry = &d;
    if (!emulate(f, base, NULL, dry_rd, dry_wr, 0) || d.refused) {
        t_dry = NULL;
        return 0;
    }
    t_dry = NULL;
    return emulate(f, base, NULL, recomp_guest_read, recomp_guest_write, 1);
#endif
}

#if !defined(_WIN32)
/* What the guest asked for on every 4 KB page the element touches. */
static int side_table_allows(uintptr_t host, int size, int is_write, void *arg)
{
    uintptr_t p, end = host + (uintptr_t)(size > 0 ? size : 1);
    (void)arg;
    for (p = host & ~(uintptr_t)0xFFF; p < end; p += 0x1000) {
        DWORD prot = w32_page_protection((const void *)p);
        switch (prot & 0xFF) {
        case 0:
        case PAGE_NOACCESS:
        case PAGE_EXECUTE:
            return 0;
        case PAGE_READONLY:
        case PAGE_EXECUTE_READ:
            if (is_write) return 0;
            break;
        default:
            break;
        }
    }
    return 1;
}
#endif

int recomp_fault_passthrough(recomp_fault *f)
{
#if defined(_WIN32)
    (void)f;
    return 0;
#else
    if (f->kind != RECOMP_FAULT_ACCESS || !s_guest_base
        || !w32_in_arena((const void *)f->host_addr))
        return 0;
    /* The faulting element itself first: if the guest page refuses it, this
     * is a real trap (or a real bug), not collateral. */
    if (!side_table_allows(f->host_addr, 1, f->is_write == 1, NULL))
        return 0;
    return recomp_fault_complete_guest(f, side_table_allows, NULL);
#endif
}

void recomp_fault_insn_text(const recomp_fault *f, char *buf, size_t n)
{
    size_t used = 0;
    if (!n)
        return;
    buf[0] = 0;
    if (!f->pc)
        return;
#if defined(__aarch64__)
    snprintf(buf, n, "%08X", *(const uint32_t *)f->pc);
    (void)used;
#else
    for (int i = 0; i < 12 && used + 4 < n; i++)
        used += (size_t)snprintf(buf + used, n - used, "%s%02X", i ? " " : "",
                                 ((const uint8_t *)f->pc)[i]);
#endif
}
