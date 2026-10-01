/* A/B runner for tools/codegen_ab (POSIX: Linux, WSL, macOS).
 *
 * Two builds of the same guest functions -- lifted without and with a set of
 * perf options -- are linked here as ab_table_a[] and ab_table_b[]. For every
 * function and every seeded state, each build runs in its own forked child
 * from an identical guest machine, and reports:
 *
 *   - every guest register the runtime keeps (GPRs, g_ebp/g_seh_ebp, the x87
 *     stack and top, XMM, MMX, DF), as hex;
 *   - every guest page it changed, by index and content hash;
 *   - a hash of the machine state at each call it made (callees are stubs
 *     that record and return), so a register cache that is stale at a call
 *     shows up even when the function later repairs it;
 *   - or how it stopped instead: a fault (signal and guest address) or a
 *     timeout.
 *
 * The parent compares nothing; it prints both results and tools/codegen_ab
 * diffs them. Forking gives each run a copy-on-write snapshot of the pristine
 * machine for free, and keeps a faulting or looping function from taking the
 * run with it.
 *
 * Guest memory: 4 GB + a guard, reserved PROT_NONE, with one read-write
 * window [AB_LO, AB_HI) holding the seeded data, the stack and (with --xbe)
 * the title's own sections. Anything outside it faults, in both builds.
 */
#define _GNU_SOURCE
/* For RecompMmx and the g_mm registers, which the header declares only for
 * generated code. Nothing below uses the bare register names. */
#define RECOMP_GENERATED_CODE
#include "recomp_types.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef MAP_NORESERVE
#define MAP_NORESERVE 0
#endif

/* ── the runtime the lifted code links against ─────────────────────────── */

RECOMP_TLS uint32_t g_eax, g_ecx, g_edx, g_esp, g_ebx, g_esi, g_edi;
RECOMP_TLS uint32_t g_ebp, g_seh_ebp, g_fs_base;
RECOMP_TLS double g_fp_stack[8];
RECOMP_TLS int g_fp_top;
RECOMP_TLS int g_df;
RECOMP_TLS uint16_t g_fp_control_word = 0x027F;
RECOMP_TLS int g_fp_cmp;
RECOMP_TLS uint16_t g_fp_cc;
RECOMP_TLS RecompXmm g_xmm0, g_xmm1, g_xmm2, g_xmm3, g_xmm4, g_xmm5, g_xmm6, g_xmm7;
RECOMP_TLS RecompMmx g_mm0, g_mm1, g_mm2, g_mm3, g_mm4, g_mm5, g_mm6, g_mm7;
RECOMP_TLS uint32_t g_icall_saved_esp, g_icall_dispatch_form;
ptrdiff_t g_xbox_mem_offset;
uint32_t g_xbox_code_lo, g_xbox_code_hi;
int g_force_return;
volatile uint32_t g_icall_trace[ICALL_TRACE_SIZE];
volatile uint32_t g_icall_trace_idx;
volatile uint64_t g_icall_count;
volatile uint64_t g_icall_guard_hits, g_icall_guard_misses;

static uint64_t s_calls;          /* rolling hash of every call boundary */
static uint32_t s_call_count;

static uint64_t mix(uint64_t h, uint64_t v)
{
    h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
    return h * 0xFF51AFD7ED558CCDull;
}

static uint64_t hash_bytes(uint64_t h, const void *p, size_t n)
{
    const uint8_t *b = (const uint8_t *)p;
    size_t i;
    for (i = 0; i + 8 <= n; i += 8) {
        uint64_t w;
        memcpy(&w, b + i, 8);
        h = mix(h, w);
    }
    for (; i < n; i++) h = mix(h, b[i]);
    return h;
}

/* What a call site exposes to its callee: the registers, the top of the
 * guest stack (return address and arguments) and the SSE/x87 state that
 * crosses calls. */
static void record_call(uint32_t va)
{
    const uint8_t *stack = (const uint8_t *)(g_xbox_mem_offset + (uint32_t)g_esp);
    uint32_t regs[9] = {g_eax, g_ecx, g_edx, g_ebx, g_esi, g_edi, g_esp,
                        g_ebp, va};
    s_calls = hash_bytes(s_calls, regs, sizeof regs);
    s_calls = hash_bytes(s_calls, stack, 16);
    s_calls = hash_bytes(s_calls, &g_xmm0, sizeof(RecompXmm) * 1);
    s_calls = hash_bytes(s_calls, g_fp_stack, sizeof g_fp_stack);
    s_calls = mix(s_calls, (uint64_t)g_fp_top);
    s_call_count++;
    /* Behave like a small cdecl callee: pop the return address and hand
     * back something that depends on what it was given. */
    g_esp += 4;
    g_eax = (uint32_t)s_calls;
    g_ecx = (uint32_t)(s_calls >> 32);
}

/* Every guest call target the builds reference gets one of these (generated
 * by tools/codegen_ab as ab_stubs.c): it records the boundary and returns. */
void ab_stub(uint32_t va) { record_call(va); }

static uint32_t s_icall_va;
static void ab_icall_target(void) { record_call(0xC0000000u | s_icall_va); }
recomp_func_t recomp_lookup_manual(uint32_t va) { (void)va; return NULL; }
recomp_func_t recomp_lookup(uint32_t va) { s_icall_va = va; return ab_icall_target; }
recomp_func_t recomp_lookup_kernel(uint32_t va) { (void)va; return NULL; }
void recomp_icall_fail_log(uint32_t va) { (void)va; }
void recomp_icall_not_code_log(uint32_t va, uint32_t esp) { (void)va; (void)esp; }
void recomp_stub_missing(uint32_t va) { record_call(0xD0000000u | va); g_esp -= 4; }
void recomp_int3_reached(uint32_t va) { record_call(0xE0000000u | va); g_esp -= 4; }
void recomp_cxx_throw(uint32_t o, uint32_t t) { (void)o; (void)t; }
void recomp_debug_service(uint32_t s, uint32_t a) { (void)s; (void)a; }
void recomp_unimpl(const char *text, uint32_t va) { (void)text; (void)va; }
uint64_t xbox_ReadTimeStampCounter(void) { return 0x123456789ull; }
void recomp_trace_enter(const char *n, uint32_t va) { (void)n; (void)va; }
void recomp_trace_exit(const char *n, uint32_t va) { (void)n; (void)va; }
void recomp_trace_esp(const char *n, const char *t) { (void)n; (void)t; }
static jmp_buf s_jmp;
jmp_buf *recomp_setjmp_slot(uint32_t va) { (void)va; return &s_jmp; }
int recomp_guest_longjmp(uint32_t va, uint32_t v) { (void)va; (void)v; return 0; }
void recomp_set_foreign_longjmp(recomp_foreign_longjmp_fn fn) { (void)fn; }
void recomp_abi_violation_log(uint32_t va, uint32_t a, uint32_t b, uint32_t c,
                              uint32_t d) { (void)va; (void)a; (void)b; (void)c; (void)d; }
void recomp_abi_pop_violation_log(uint32_t va, uint32_t a, uint32_t b, uint32_t c,
                                  uint32_t d, uint32_t e)
{ (void)va; (void)a; (void)b; (void)c; (void)d; (void)e; }

/* ── the functions under test (ab_a.c / ab_b.c) ────────────────────────── */

typedef struct { const char *name; uint32_t va; void (*fn)(void); } AbEntry;
extern const AbEntry ab_table_a[], ab_table_b[];
extern const int ab_count;

/* ── the guest machine ─────────────────────────────────────────────────── */

#define AB_LO      0x00010000u
#define AB_HI      0x01000000u
#define AB_HEAP    0x00800000u          /* seeded pointer targets */
#define AB_HEAP_HI 0x00C00000u
#define AB_STACK   0x00F00000u          /* esp at entry */
#define AB_FS      0x00FF0000u
#define PAGE       4096u

static uint8_t *s_pristine;             /* [AB_LO, AB_HI) before the run */
static uint8_t *s_image;                /* --xbe sections, or NULL */
static size_t s_image_len;

static uint64_t s_rng;
static uint64_t s_seed;          /* --seed, mixed into every machine */
static uint32_t rnd(void)
{
    s_rng ^= s_rng << 13; s_rng ^= s_rng >> 7; s_rng ^= s_rng << 17;
    return (uint32_t)(s_rng >> 16);
}

/* Values that sit on an edge at some width, small counts, and pointers into
 * the seeded heap -- so a function that chases a pointer keeps landing on
 * mapped, seeded memory. */
static uint32_t pool(void)
{
    static const uint32_t edges[] = {
        0, 1, 2, 3, 4, 7, 8, 0x10, 0x1F, 0x20, 0x7F, 0x80, 0xFF, 0x100,
        0x7FFF, 0x8000, 0xFFFF, 0x10000, 0x7FFFFFFF, 0x80000000u,
        0xFFFFFFFFu, 0xFFFFFFFEu, 0xFFFFFF80u, 0x3F800000u, 0xBF800000u,
        0x7FC00000u, 0x00800000u};
    uint32_t r = rnd() % 16;
    if (r < 7)
        return AB_HEAP + 0x1000u + (rnd() % ((AB_HEAP_HI - AB_HEAP) - 0x2000u) & ~3u);
    if (r < 12)
        return edges[rnd() % (sizeof edges / sizeof edges[0])];
    if (r < 14)
        return rnd() % 64;
    return rnd();
}

/* Guest memory for one state: shared by every function, so seeded once. */
static void seed_memory(uint8_t *g, unsigned state)
{
    uint32_t a;
    s_rng = (0x9E3779B97F4A7C15ull ^ (state * 0x2545F491u) ^ 1) + s_seed * 0xA24BAED4963EE407ull;
    for (a = AB_LO; a < AB_HI; a += 4) {
        uint32_t v = pool();
        memcpy(g + a, &v, 4);
    }
    if (s_image) {
        /* (u32 va, u32 len, bytes)*: the title's sections over the noise */
        size_t off = 0;
        while (off + 8 <= s_image_len) {
            uint32_t va, len;
            memcpy(&va, s_image + off, 4);
            memcpy(&len, s_image + off + 4, 4);
            off += 8;
            if (va >= AB_LO && va + len <= AB_HI && off + len <= s_image_len)
                memcpy(g + va, s_image + off, len);
            off += len;
        }
    }
}

/* Registers for one (function, state): cheap, so varied per function. */
static void seed_registers(unsigned fn_index, unsigned state)
{
    int i;
    s_rng = (0xD1B54A32D192ED03ull ^ ((uint64_t)fn_index << 32) ^ (state * 0x9E3779B9u) ^ 7) + s_seed * 0x9FB21C651E98DF25ull;
    g_eax = pool(); g_ecx = pool(); g_edx = pool();
    /* esi, edi and ebx always point into the seeded heap: the corpus
     * generator addresses through exactly these three. */
    g_esi = AB_HEAP + 0x2000u + (rnd() % 0x100000u & ~3u);
    g_edi = AB_HEAP + 0x180000u + (rnd() % 0x100000u & ~3u);
    g_ebx = AB_HEAP + 0x300000u + (rnd() % 0x80000u & ~3u);
    g_esp = AB_STACK - 0x40u - 4u * (rnd() % 8);
    g_ebp = g_esp + 0x40u;
    g_seh_ebp = g_ebp;
    g_fs_base = AB_FS;
    g_df = (rnd() % 8 == 0);
    g_fp_top = 0;
    for (i = 0; i < 8; i++)
        g_fp_stack[i] = (double)(int32_t)pool() / (double)(1 + rnd() % 100);
    {
        RecompXmm *x[8] = {&g_xmm0, &g_xmm1, &g_xmm2, &g_xmm3,
                           &g_xmm4, &g_xmm5, &g_xmm6, &g_xmm7};
        RecompMmx *m[8] = {&g_mm0, &g_mm1, &g_mm2, &g_mm3,
                           &g_mm4, &g_mm5, &g_mm6, &g_mm7};
        static const float special[] = {0.0f, -0.0f, 1.0f, -1.0f, 0.5f,
                                        1e30f, -1e-30f, 3.0f};
        for (i = 0; i < 8; i++) {
            int l;
            for (l = 0; l < 4; l++) {
                if (rnd() % 4 == 0)
                    x[i]->f[l] = special[rnd() % 8];
                else
                    x[i]->f[l] = (float)(int32_t)(rnd() % 2001 - 1000) / 7.0f;
            }
            m[i]->ud[0] = pool(); m[i]->ud[1] = pool();
        }
    }
    s_calls = 0x51ED270B27u;
    s_call_count = 0;
}

/* ── one run, in a child ───────────────────────────────────────────────── */

static int s_out = -1;
static int s_dump;                /* also print every changed page in full */
static uint8_t *s_guest;

static void put(const char *text)
{
    size_t n = strlen(text);
    while (n) {
        ssize_t w = write(s_out, text, n);
        if (w <= 0) _exit(3);
        text += w; n -= (size_t)w;
    }
}

static void on_signal(int sig, siginfo_t *si, void *uc)
{
    char buf[96];
    uintptr_t a = (uintptr_t)si->si_addr, base = (uintptr_t)s_guest;
    (void)uc;
    if (sig == SIGALRM)
        snprintf(buf, sizeof buf, "TIMEOUT\n");
    else if (a >= base && a < base + 0x100010000ull)
        snprintf(buf, sizeof buf, "FAULT sig=%d guest=%08X\n", sig, (unsigned)(a - base));
    else
        snprintf(buf, sizeof buf, "FAULT sig=%d host\n", sig);
    put(buf);
    _exit(0);
}

static void report(void)
{
    char buf[512];
    uint32_t a, dirty = 0;
    uint64_t pages = 0x2D358DCCAA6C78A5ull;
    int i;
    RecompXmm *x[8] = {&g_xmm0, &g_xmm1, &g_xmm2, &g_xmm3,
                       &g_xmm4, &g_xmm5, &g_xmm6, &g_xmm7};
    RecompMmx *m[8] = {&g_mm0, &g_mm1, &g_mm2, &g_mm3,
                       &g_mm4, &g_mm5, &g_mm6, &g_mm7};

    snprintf(buf, sizeof buf,
             "OK eax=%08X ecx=%08X edx=%08X ebx=%08X esi=%08X edi=%08X "
             "esp=%08X ebp=%08X seh_ebp=%08X df=%d fp_top=%d",
             g_eax, g_ecx, g_edx, g_ebx, g_esi, g_edi, g_esp, g_ebp,
             g_seh_ebp, g_df, g_fp_top);
    put(buf);
    for (i = 0; i < 8; i++) {
        uint64_t b;
        memcpy(&b, &g_fp_stack[i], 8);
        snprintf(buf, sizeof buf, " st%d=%016llX", i, (unsigned long long)b);
        put(buf);
    }
    for (i = 0; i < 8; i++) {
        snprintf(buf, sizeof buf, " xmm%d=%08X%08X%08X%08X", i,
                 x[i]->u[3], x[i]->u[2], x[i]->u[1], x[i]->u[0]);
        put(buf);
    }
    for (i = 0; i < 8; i++) {
        snprintf(buf, sizeof buf, " mm%d=%016llX", i, (unsigned long long)m[i]->q);
        put(buf);
    }
    snprintf(buf, sizeof buf, " calls=%u:%016llX", s_call_count,
             (unsigned long long)s_calls);
    put(buf);
    {
        /* One pass: the summary on this line, the page list (for the report
         * when summaries differ) on the next. */
        static char list[1 << 16];
        size_t used = 0;
        for (a = AB_LO; a < AB_HI; a += PAGE) {
            if (memcmp(s_guest + a, s_pristine + (a - AB_LO), PAGE) != 0) {
                uint64_t h = hash_bytes(1, s_guest + a, PAGE);
                pages = mix(mix(pages, h), a);
                dirty++;
                if (used + 32 < sizeof list)
                    used += (size_t)snprintf(list + used, sizeof list - used,
                                             " %08X:%016llX", a,
                                             (unsigned long long)h);
            }
        }
        snprintf(buf, sizeof buf, " pages=%u:%016llX\nPAGES", dirty,
                 (unsigned long long)pages);
        put(buf);
        list[used] = 0;
        put(list);
        put("\n");
        if (s_dump) {
            /* For a mismatch: the bytes themselves, so the report can tell a
             * NaN payload from a real difference. One line per page. */
            static char hex[PAGE * 2 + 1];
            static const char digits[] = "0123456789ABCDEF";
            for (a = AB_LO; a < AB_HI; a += PAGE) {
                uint32_t k;
                if (memcmp(s_guest + a, s_pristine + (a - AB_LO), PAGE) == 0)
                    continue;
                for (k = 0; k < PAGE; k++) {
                    hex[2 * k] = digits[s_guest[a + k] >> 4];
                    hex[2 * k + 1] = digits[s_guest[a + k] & 15];
                }
                hex[2 * PAGE] = 0;
                snprintf(buf, sizeof buf, "DUMP %08X ", a);
                put(buf);
                put(hex);
                put("\n");
            }
        }
    }
}

static void run_child(void (*fn)(void), int fd, unsigned timeout_s)
{
    struct sigaction sa;
    stack_t ss;
    static uint8_t altstack[65536];
    int sigs[] = {SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGTRAP, SIGALRM};
    unsigned k;

    s_out = fd;
    ss.ss_sp = altstack; ss.ss_size = sizeof altstack; ss.ss_flags = 0;
    sigaltstack(&ss, NULL);
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_signal;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    for (k = 0; k < sizeof sigs / sizeof sigs[0]; k++)
        sigaction(sigs[k], &sa, NULL);
    alarm(timeout_s);
    fn();
    alarm(0);
    report();
    _exit(0);
}

/* Runs fn in a child and prints what it reported; returns 1 when the run
 * returned normally (an "OK" report). */
static int run_one(void (*fn)(void), unsigned timeout_s)
{
    int p[2], ok = -1;
    pid_t pid;
    char buf[8192];
    ssize_t n;

    fflush(stdout);
    if (pipe(p) != 0) { perror("pipe"); exit(1); }
    pid = fork();
    if (pid < 0) { perror("fork"); exit(1); }
    if (pid == 0) {
        close(p[0]);
        run_child(fn, p[1], timeout_s);
    }
    close(p[1]);
    while ((n = read(p[0], buf, sizeof buf)) > 0) {
        if (ok < 0) ok = (n >= 2 && buf[0] == 'O' && buf[1] == 'K');
        fwrite(buf, 1, (size_t)n, stdout);
    }
    close(p[0]);
    {
        int status = 0;
        waitpid(pid, &status, 0);
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            printf("CRASH status=%d\n", status);
            ok = 0;
        }
    }
    return ok > 0;
}

int main(int argc, char **argv)
{
    unsigned states = argc > 1 ? (unsigned)atoi(argv[1]) : 8;
    unsigned timeout_s = argc > 2 ? (unsigned)atoi(argv[2]) : 2;
    int only = argc > 3 ? atoi(argv[3]) : -1;
    const char *image = argc > 4 ? argv[4] : NULL;
    s_seed = argc > 5 ? (uint64_t)strtoull(argv[5], NULL, 10) : 0;
    s_dump = argc > 6 ? atoi(argv[6]) : 0;
    int i;
    unsigned s;

    s_guest = (uint8_t *)mmap(NULL, 0x100010000ull, PROT_NONE,
                              MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (s_guest == MAP_FAILED) { perror("mmap 4 GB"); return 1; }
    if (mprotect(s_guest + AB_LO, AB_HI - AB_LO, PROT_READ | PROT_WRITE) != 0) {
        perror("mprotect"); return 1;
    }
    g_xbox_mem_offset = (ptrdiff_t)s_guest;
    s_pristine = (uint8_t *)malloc(AB_HI - AB_LO);
    if (image && *image) {
        FILE *f = fopen(image, "rb");
        if (!f) { perror(image); return 1; }
        fseek(f, 0, SEEK_END);
        s_image_len = (size_t)ftell(f);
        fseek(f, 0, SEEK_SET);
        s_image = (uint8_t *)malloc(s_image_len);
        if (fread(s_image, 1, s_image_len, f) != s_image_len) { perror("read"); return 1; }
        fclose(f);
    }

    /* State-major: the memory is seeded once per state and every run is a
     * child, so the parent's copy stays pristine for all of them. A and B
     * start from the same registers too: they are set here, in the parent,
     * and only ever changed in the children. */
    for (s = 0; s < states; s++) {
        seed_memory(s_guest, s);
        memcpy(s_pristine, s_guest + AB_LO, AB_HI - AB_LO);
        for (i = 0; i < ab_count; i++) {
            if (only >= 0 && i != only) continue;
            seed_registers((unsigned)i, s);
            printf("RUN %d %u %s\nA ", i, s, ab_table_a[i].name);
            /* Only a run A returns from is compared (see compare() in
             * __main__.py), so B is not run when A faulted or timed out. */
            if (run_one(ab_table_a[i].fn, timeout_s)) {
                printf("B ");
                run_one(ab_table_b[i].fn, timeout_s);
            }
        }
    }
    return 0;
}
