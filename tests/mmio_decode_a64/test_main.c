/*
 * mmio_decode_a64 - does the AArch64 trapped-access emulator do exactly what
 * the hardware does?
 *
 * The hardware is the oracle. Each snippet below is assembled by the
 * compiler into a table, copied into an executable stub that loads x0..x15
 * and v0..v7 from a state block, runs the snippet, and stores them back. It
 * runs twice over identical memory: once against an ordinary read-write
 * view, and once against a second view of the same shared memory that is
 * PROT_NONE, so every access faults and the signal handler completes it with
 * mmio_decode_a64.h through the read-write view. Registers and memory must
 * come out the same -- registers that hold an address into the buffer
 * differing by exactly the distance between the two views.
 *
 * Then the same comparison for C compiled the way the lifted code is
 * (volatile accesses through a base + 32-bit guest address), so whatever
 * clang chooses to emit for MEM8..MEM64, memcpy and memset is covered too,
 * not only the forms listed here.
 *
 * arm64 macOS and Linux only. Needs no game files.
 */
#if !defined(__aarch64__)
#include <stdio.h>
int main(void) { printf("mmio_decode_a64: not an arm64 host, skipped\n"); return 0; }
#else

#define _GNU_SOURCE
#include "mmio_decode_a64.h"

#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <libkern/OSCacheControl.h>
#endif

static int failures;
#define CHECK(name, cond) \
    do { if (cond) {} else { printf("FAIL: %s (line %d)\n", name, __LINE__); failures++; } } while (0)

/* ---- the snippets ------------------------------------------------------ */

/* x2 is the base pointer (the middle of the buffer), x3 a small positive
 * index, x4 a negative one; x9 and x10 are scratch the snippets may point
 * into the buffer. Snippets are separated by 0xFFFFFFFF and the table ends
 * with 0. */
__asm__(
    ".data\n"
    ".p2align 2\n"
    ".globl _a64_snippets\n.globl a64_snippets\n"
    "_a64_snippets:\na64_snippets:\n"
    ".arch_extension lse\n"
    ".arch_extension rcpc\n"
#define S(x) x "\n.word 0xFFFFFFFF\n"
    /* integer loads, every addressing mode */
    S("ldr w1, [x2]")              S("ldr x1, [x2, #8]")
    S("ldr w1, [x2, x3]")          S("ldr w1, [x2, w3, uxtw]")
    S("ldr x1, [x2, w3, uxtw #3]") S("ldr w1, [x2, w4, sxtw]")
    S("ldr x1, [x2, x4, lsl #3]")  S("ldr x1, [x2, x4, sxtx]")
    S("ldr w1, [x2, w3, uxtw #2]") S("ldrh w1, [x2, w3, uxtw #1]")
    S("ldrb w1, [x2, #3]")         S("ldrh w1, [x2, #6]")
    S("ldrsb w1, [x2, #5]")        S("ldrsb x1, [x2, #5]")
    S("ldrsh w1, [x2, #2]")        S("ldrsh x1, [x2, #2]")
    S("ldrsw x1, [x2, #4]")        S("ldrsw x1, [x2, x3]")
    S("ldrsb w1, [x2, x3]")        S("ldrsh x1, [x2, w4, sxtw #1]")
    S("ldur w1, [x2, #-3]")        S("ldur x1, [x2, #-9]")
    S("ldurb w1, [x2, #-1]")       S("ldursh w1, [x2, #-7]")
    S("ldursw x1, [x2, #-13]")
    S("ldr w1, [x2, #4]!")         S("ldr x1, [x2], #-8")
    S("ldrb w1, [x2, #-1]!")       S("ldrsh x1, [x2], #2")
    S("ldr wzr, [x2, w3, uxtw]")   /* a volatile read whose value is unused */
    /* integer stores */
    S("str w1, [x2]")              S("str x1, [x2, #16]")
    S("strb w1, [x2, #1]")         S("strh w1, [x2, #2]")
    S("str wzr, [x2, w3, uxtw]")   S("str xzr, [x2]")
    S("stur w1, [x2, #-5]")        S("sturh w1, [x2, #-3]")
    S("str w1, [x2, x3, lsl #2]")  S("str x1, [x2, w4, sxtw #3]")
    S("str w1, [x2, #-4]!")        S("str x1, [x2], #16")
    S("strb w1, [x2, x3]")         S("strh wzr, [x2, #8]")
    /* pairs */
    S("ldp w1, w5, [x2]")          S("ldp x1, x5, [x2, #-16]")
    S("ldp x1, x5, [x2, #16]!")    S("ldp x1, x5, [x2], #-32")
    S("ldpsw x1, x5, [x2, #8]")    S("stp w1, w5, [x2, #4]")
    S("stp x1, x5, [x2, #-64]!")   S("stp x1, x5, [x2], #48")
    S("ldnp x1, x5, [x2]")         S("stnp w1, w5, [x2]")
    S("stp xzr, xzr, [x2, #32]")
    /* FP / SIMD */
    S("ldr s0, [x2]")              S("ldr d0, [x2, #8]")
    S("ldr q0, [x2, #16]")         S("ldur q1, [x2, #12]")
    S("stur q1, [x2, #12]")        S("str s0, [x2, w3, uxtw]")
    S("str d0, [x2, x3]")          S("ldr d1, [x2, x3, lsl #3]")
    S("ldr s1, [x2, w3, uxtw #2]") S("ldr b0, [x2, #1]")
    S("ldr h0, [x2, #2]")          S("str h1, [x2, #6]")
    S("str b1, [x2, #7]")          S("ldr s0, [x2, #20]!")
    S("ldr s1, [x2], #4")          S("str q2, [x2, #-32]")
    S("ldr q3, [x2, x3]")          S("str q3, [x2, x3, lsl #4]")
    S("ldp s0, s1, [x2]")          S("ldp d0, d1, [x2, #8]")
    S("ldp q0, q1, [x2, #-32]")    S("stp q0, q1, [x2, #32]")
    S("stp d2, d3, [x2], #16")     S("ldp q4, q5, [x2, #64]!")
    /* atomics, acquire/release, exclusives */
    S("ldadd w1, w5, [x2]")        S("ldaddal x1, x5, [x2]")
    S("ldclr w1, w5, [x2]")        S("ldeor x1, x5, [x2]")
    S("ldset w1, w5, [x2]")        S("ldsmax w1, w5, [x2]")
    S("ldsmin x1, x5, [x2]")       S("ldumax w1, w5, [x2]")
    S("ldumin x1, x5, [x2]")       S("swp w1, w5, [x2]")
    S("swpal x1, x5, [x2]")        S("ldaddb w1, w5, [x2]")
    S("ldaddh w1, w5, [x2]")       S("ldaddal w1, wzr, [x2]")
    S("cas w6, w5, [x2]")          S("casal x7, x5, [x2]")
    S("cas x6, x5, [x2]")          S("casb w6, w5, [x2]")
    S("ldar w1, [x2]")             S("stlr x1, [x2]")
    S("ldarb w1, [x2]")            S("stlrh w1, [x2]")
    S("ldapr w1, [x2]")
    S("ldxr w1, [x2]\nstxr w8, w5, [x2]")
    S("ldaxr x1, [x2]\nstlxr w8, x5, [x2]")
    /* what memset does to large runs */
    S("add x9, x2, #128\ndc zva, x9")
#undef S
    ".word 0\n"
    ".text\n");
extern const uint32_t a64_snippets[];

/* The stub: x16 = state, load, run, store, return. */
__asm__(
    ".data\n.p2align 2\n"
    ".globl _a64_pro\n.globl a64_pro\n.globl _a64_epi\n.globl a64_epi\n"
    ".globl _a64_epi_end\n.globl a64_epi_end\n"
    "_a64_pro:\na64_pro:\n"
    "stp x29, x30, [sp, #-16]!\n"
    "mov x16, x0\n"
    "ldp x0, x1, [x16, #0]\n  ldp x2, x3, [x16, #16]\n"
    "ldp x4, x5, [x16, #32]\n ldp x6, x7, [x16, #48]\n"
    "ldp x8, x9, [x16, #64]\n ldp x10, x11, [x16, #80]\n"
    "ldp x12, x13, [x16, #96]\n ldp x14, x15, [x16, #112]\n"
    "ldp q0, q1, [x16, #128]\n ldp q2, q3, [x16, #160]\n"
    "ldp q4, q5, [x16, #192]\n ldp q6, q7, [x16, #224]\n"
    "_a64_epi:\na64_epi:\n"
    "stp x0, x1, [x16, #0]\n  stp x2, x3, [x16, #16]\n"
    "stp x4, x5, [x16, #32]\n stp x6, x7, [x16, #48]\n"
    "stp x8, x9, [x16, #64]\n stp x10, x11, [x16, #80]\n"
    "stp x12, x13, [x16, #96]\n stp x14, x15, [x16, #112]\n"
    "stp q0, q1, [x16, #128]\n stp q2, q3, [x16, #160]\n"
    "stp q4, q5, [x16, #192]\n stp q6, q7, [x16, #224]\n"
    "ldp x29, x30, [sp], #16\n"
    "ret\n"
    "_a64_epi_end:\na64_epi_end:\n"
    ".text\n");
extern const uint32_t a64_pro[], a64_epi[], a64_epi_end[];

typedef struct { uint64_t x[16]; uint64_t v[8][2]; } stub_state;

/* ---- two views of one buffer ------------------------------------------- */

#define BUF 0x10000u
static uint8_t *s_rw;      /* always read-write */
static uint8_t *s_trap;    /* the same bytes, PROT_NONE while a run is trapped */
static volatile int s_emulated, s_refused;

static uint64_t rd(void *dev, uint32_t off, int size)
{
    uint64_t v = 0;
    (void)dev;
    memcpy(&v, s_rw + off, (size_t)size);
    return v;
}

static void wr(void *dev, uint32_t off, uint64_t v, int size)
{
    (void)dev;
    memcpy(s_rw + off, &v, (size_t)size);
}

static void on_fault(int sig, siginfo_t *si, void *ucv)
{
    ucontext_t *uc = (ucontext_t *)ucv;
    uintptr_t far = (uintptr_t)si->si_addr;
    mmio_a64_ctx c;

    if (far >= (uintptr_t)s_trap && far < (uintptr_t)s_trap + BUF) {
        mmio_a64_from_ucontext(&c, uc);
        if (mmio_a64_emulate(&c, *(const uint32_t *)c.pc, (uintptr_t)s_trap,
                             NULL, rd, wr)) {
            mmio_a64_to_ucontext(&c, uc);
            s_emulated++;
            return;
        }
        s_refused++;
        printf("  refused: %08X at fault %p\n", *(const uint32_t *)c.pc, (void *)far);
    }
    signal(sig, SIG_DFL);   /* not ours, or not understood: die on return */
}

static void fill(void)
{
    for (unsigned i = 0; i < BUF; i++)
        s_rw[i] = (uint8_t)(i * 37u + 11u);
}

/* ---- the executable stub ------------------------------------------------ */

static uint32_t *s_code;
static size_t    s_code_cap = 0x4000;

static void (*build_stub(const uint32_t *snip, size_t n))(stub_state *)
{
    size_t pro = (size_t)(a64_epi - a64_pro), epi = (size_t)(a64_epi_end - a64_epi);
#if defined(__APPLE__)
    pthread_jit_write_protect_np(0);
#endif
    memcpy(s_code, a64_pro, pro * 4);
    memcpy(s_code + pro, snip, n * 4);
    memcpy(s_code + pro + n, a64_epi, epi * 4);
#if defined(__APPLE__)
    pthread_jit_write_protect_np(1);
    sys_icache_invalidate(s_code, (pro + n + epi) * 4);
#else
    __builtin___clear_cache((char *)s_code, (char *)(s_code + pro + n + epi));
#endif
    return (void (*)(stub_state *))(void *)s_code;
}

static void init_state(stub_state *s, uint8_t *view)
{
    for (int i = 0; i < 16; i++)
        s->x[i] = 0x0123456789ABCDEFull * (uint64_t)(i + 1) ^ 0xF0F0F0F00F0F0F0Full;
    s->x[2] = (uint64_t)(uintptr_t)(view + BUF / 2);
    s->x[3] = 0x18;
    s->x[4] = (uint64_t)-16;
    /* x6 equals the word at the base, so the CAS snippets that compare
     * against it succeed and the x7 ones fail. */
    memcpy(&s->x[6], s_rw + BUF / 2, 8);
    for (int i = 0; i < 8; i++) {
        s->v[i][0] = 0x1111111111111111ull * (uint64_t)(i + 1);
        s->v[i][1] = 0x2222222222222222ull * (uint64_t)(i + 3) ^ 0x5A5A5A5Aull;
    }
}

/* Registers holding an address in the buffer follow the view they ran on. */
static int same_reg(uint64_t plain, uint64_t trapped)
{
    uintptr_t lo = (uintptr_t)s_rw, d = (uintptr_t)s_trap - (uintptr_t)s_rw;
    if (plain >= lo && plain < lo + BUF)
        return trapped == plain + d;
    return trapped == plain;
}

static void run_snippets(void)
{
    static uint8_t want[BUF];
    const uint32_t *p = a64_snippets;
    int count = 0;

    while (*p) {
        const uint32_t *start = p;
        size_t n;
        stub_state a, b;
        void (*fn)(stub_state *);
        char name[64];

        while (*p != 0xFFFFFFFFu) p++;
        n = (size_t)(p - start);
        p++;
        snprintf(name, sizeof(name), "snippet %d (%08X)", count, start[0]);
        count++;

        fn = build_stub(start, n);

        /* The oracle: the hardware on plain memory. */
        fill();
        mprotect(s_trap, BUF, PROT_READ | PROT_WRITE);
        init_state(&a, s_rw);
        fn(&a);
        memcpy(want, s_rw, BUF);

        /* The same on a view where every access faults. */
        fill();
        init_state(&b, s_trap);
        mprotect(s_trap, BUF, PROT_NONE);
        s_emulated = 0;
        fn(&b);
        mprotect(s_trap, BUF, PROT_READ | PROT_WRITE);

        {
            int ok = 1;
            for (int i = 0; i < 16; i++)
                if (!same_reg(a.x[i], b.x[i])) {
                    printf("  %s: x%d plain %016llX trapped %016llX\n", name, i,
                           (unsigned long long)a.x[i], (unsigned long long)b.x[i]);
                    ok = 0;
                }
            if (memcmp(a.v, b.v, sizeof(a.v))) {
                printf("  %s: SIMD registers differ\n", name);
                ok = 0;
            }
            if (memcmp(want, s_rw, BUF)) {
                printf("  %s: memory differs\n", name);
                ok = 0;
            }
            if (!s_emulated) {
                printf("  %s: never faulted\n", name);
                ok = 0;
            }
            CHECK(name, ok);
        }
    }
    printf("mmio_decode_a64: %d snippets compared against the hardware\n", count);
}

/* ---- C compiled like the lifted code ------------------------------------ */

static uintptr_t g_off;   /* g_xbox_mem_offset's stand-in */
#define T8(a)  (*(volatile uint8_t  *)((uintptr_t)(uint32_t)(a) + g_off))
#define T16(a) (*(volatile uint16_t *)((uintptr_t)(uint32_t)(a) + g_off))
#define T32(a) (*(volatile uint32_t *)((uintptr_t)(uint32_t)(a) + g_off))
#define T64(a) (*(volatile uint64_t *)((uintptr_t)(uint32_t)(a) + g_off))
#define TF(a)  (*(volatile float    *)((uintptr_t)(uint32_t)(a) + g_off))
#define TD(a)  (*(volatile double   *)((uintptr_t)(uint32_t)(a) + g_off))

__attribute__((noinline)) static uint64_t lifted_like(uint32_t eax, uint32_t ecx)
{
    uint64_t sum = 0;
    for (uint32_t i = 0; i < 64; i++) {
        uint32_t a = 0x100u + ((eax + i * 12u) & 0x3FFCu);
        sum += T32(a);
        T32(a + 4) = (uint32_t)sum ^ ecx;
        sum += (int8_t)T8(a + 1) + (int16_t)T16(a + 2);
        T16(a + 6) = (uint16_t)(T16(a + 6) + 3u);
        T8(a + 9) |= 0x40;
        TF(a + 12) = TF(a + 12) * 0.5f + 1.0f;
        TD(a + 16) = TD(a + 16) + (double)i;
        T64(a + 24) ^= sum;
    }
    return sum;
}

__attribute__((noinline)) static void lib_like(uint8_t *mem)
{
    /* memcpy and memset as the runtime calls them on guest memory: these
     * are where Q-register pairs and DC ZVA come from. */
    memcpy(mem + 0x2000, mem + 0x100, 1500);
    memset(mem + 0x3000, 0, 4096);
    memmove(mem + 0x105, mem + 0x100, 333);
}

static void run_compiled(void)
{
    static uint8_t want[BUF];
    uint64_t a, b;

    fill();
    mprotect(s_trap, BUF, PROT_READ | PROT_WRITE);
    g_off = (uintptr_t)s_rw;
    a = lifted_like(0x1234, 0xBEEF);
    lib_like(s_rw);
    memcpy(want, s_rw, BUF);

    fill();
    g_off = (uintptr_t)s_trap;
    mprotect(s_trap, BUF, PROT_NONE);
    s_emulated = 0;
    b = lifted_like(0x1234, 0xBEEF);
    lib_like(s_trap);
    mprotect(s_trap, BUF, PROT_READ | PROT_WRITE);

    CHECK("compiled MEM accesses: same result", a == b);
    CHECK("compiled MEM accesses: same memory", memcmp(want, s_rw, BUF) == 0);
    CHECK("compiled MEM accesses: emulated", s_emulated > 0);
    printf("mmio_decode_a64: compiled code: %d accesses emulated\n", s_emulated);
}

/* ---- the decoder on its own: device offsets and refusals ---------------- */

static struct { uint32_t off; int size; uint64_t val; int reads, writes; } dev;
static uint64_t d_rd(void *d, uint32_t off, int size) { (void)d; dev.off = off; dev.size = size; dev.reads++; return 0xFFFFFFFF80000001ull; }
static void d_wr(void *d, uint32_t off, uint64_t v, int size) { (void)d; dev.off = off; dev.size = size; dev.val = v; dev.writes++; }

static void run_device(void)
{
    mmio_a64_ctx c;

    memset(&c, 0, sizeof(c));
    c.x[2] = 0x1000100;          /* the device's base is 0x1000000 */
    c.x[1] = 0xAABBCCDD11223344ull;
    memset(&dev, 0, sizeof(dev));
    c.pc = 0x4000;
    /* str w1, [x2, #4] = B9000441 */
    CHECK("device store handled", mmio_a64_emulate(&c, 0xB9000441u, 0x1000000, NULL, d_rd, d_wr));
    CHECK("device store offset", dev.off == 0x104 && dev.size == 4 && dev.val == 0x11223344u);
    CHECK("pc advanced", c.pc == 0x4004);
    /* ldrsw x1, [x2] = B9800041: sign-extended device read */
    CHECK("device ldrsw", mmio_a64_emulate(&c, 0xB9800041u, 0x1000000, NULL, d_rd, d_wr)
                          && c.x[1] == 0xFFFFFFFF80000001ull && dev.size == 4);
    /* ldr w1, [x2] = B9400041: zero-extended, high half cleared */
    CHECK("device ldr w", mmio_a64_emulate(&c, 0xB9400041u, 0x1000000, NULL, d_rd, d_wr)
                          && c.x[1] == 0x80000001ull);
    /* str wzr, [x10] = B900015F: the exact store TimeSplitters 2's
     * Direct3D_CreateDevice makes to PCRTC (0xFD600140). Rt = 31 is the zero
     * register for the data, so the device sees a 4-byte 0; Rn = 10 is the
     * base. (Rn = 31 would be SP; Rt = 31 never is.) */
    c.x[10] = 0x1600140;
    c.sp = 0xDEADBEEF;
    memset(&dev, 0, sizeof(dev));
    dev.val = 0x55;
    CHECK("str wzr, [x10] handled", mmio_a64_emulate(&c, 0xB900015Fu, 0x1000000, NULL, d_rd, d_wr));
    CHECK("str wzr stores zero, not SP", dev.writes == 1 && dev.val == 0 && dev.size == 4
                                         && dev.off == 0x600140);
    /* Not a load/store: add x1, x1, #1 = 91000421 */
    c.pc = 0x5000;
    CHECK("non-memory instruction refused",
          !mmio_a64_emulate(&c, 0x91000421u, 0x1000000, NULL, d_rd, d_wr) && c.pc == 0x5000);
    /* ldxp x1, x5, [x2] = C87F1441: a pair exclusive, refused */
    CHECK("exclusive pair refused", !mmio_a64_emulate(&c, 0xC87F1441u, 0x1000000, NULL, d_rd, d_wr));
    /* ESR write bit */
    CHECK("esr: data abort write", mmio_a64_esr_is_write((0x24ull << 26) | (1u << 6)) == 1);
    CHECK("esr: data abort read", mmio_a64_esr_is_write(0x25ull << 26) == 0);
    CHECK("esr: not a data abort", mmio_a64_esr_is_write(0x20ull << 26) == -1);
}

int main(void)
{
    struct sigaction sa;
    char name[64];
    int fd;

    snprintf(name, sizeof(name), "/mmio_a64.%d", (int)getpid());
    fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (fd < 0) { perror("shm_open"); return 2; }
    shm_unlink(name);
    if (ftruncate(fd, BUF) != 0) { perror("ftruncate"); return 2; }
    s_rw   = mmap(NULL, BUF, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    s_trap = mmap(NULL, BUF, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    s_code = mmap(NULL, s_code_cap, PROT_READ | PROT_WRITE | PROT_EXEC,
                  MAP_PRIVATE | MAP_ANON
#if defined(__APPLE__)
                  | MAP_JIT
#endif
                  , -1, 0);
    if (s_rw == MAP_FAILED || s_trap == MAP_FAILED || s_code == MAP_FAILED) {
        perror("mmap");
        return 2;
    }

    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = on_fault;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);

    printf("mmio_decode_a64: running\n");
    run_device();
    run_snippets();
    run_compiled();

    if (failures || s_refused) {
        printf("mmio_decode_a64: %d FAILURE(S), %d refused\n", failures, s_refused);
        return 1;
    }
    printf("mmio_decode_a64: ALL PASS\n");
    return 0;
}

#endif /* __aarch64__ */
