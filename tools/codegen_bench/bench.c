/* Driver for tools/codegen_bench: calls each lifted snippet over a fixed guest
 * state and reports ns per call plus a checksum of what it wrote.
 *
 * Every build of every variant must print the same checksums -- a variant that
 * is faster and different is a bug in the variant, and run.py fails on it.
 *
 * Guest memory is a plain 16 MB host buffer; g_xbox_mem_offset points at it, so
 * the generated MEM macros work unchanged. No kernel, no runtime library. */
#include "recomp_types.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* The register file. A header variant may gather the GPRs into one struct and
 * #define the g_ names onto it; define whichever shape it declares. */
#ifdef g_eax
RECOMP_TLS RecompGuestRegs g_regs;
#else
RECOMP_TLS uint32_t g_eax, g_ecx, g_edx, g_esp, g_ebx, g_esi, g_edi;
#endif
RECOMP_TLS uint32_t g_ebp, g_seh_ebp, g_fs_base;
RECOMP_TLS double g_fp_stack[8];
RECOMP_TLS int g_fp_top;
RECOMP_TLS int g_df;
RECOMP_TLS RecompXmm g_xmm0, g_xmm1, g_xmm2, g_xmm3, g_xmm4, g_xmm5, g_xmm6, g_xmm7;
ptrdiff_t g_xbox_mem_offset;

void sub_000A5230(void);  /* clear.s */
void sub_000F7487(void);  /* matmul.s */
void sub_00011000(void);  /* keysearch.s */
void sub_00090200(void);  /* caller.s (calls callee.s) */
void sub_00090000(void);  /* x87sum.s */

static double now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}

#define GUEST(a)  ((uint8_t *)(g_xbox_mem_offset + (uint32_t)(a)))
#define W32(a, v) (*(uint32_t *)GUEST(a) = (uint32_t)(v))
#define WF(a, v)  (*(float *)GUEST(a) = (float)(v))
#define R32(a)    (*(const uint32_t *)GUEST(a))
#define STACK_TOP 0x00F7FF00u

/* Push the arguments and a return address the way a guest `call` would, run
 * the lifted function, and check it left esp where its `ret` says it should. */
static void guest_call(void (*fn)(void), int nargs, const uint32_t *args,
                       int callee_pops)
{
    uint32_t sp = STACK_TOP - 4u * (uint32_t)(nargs + 1);
    int i;
    W32(sp, 0xDEAD0000u);
    for (i = 0; i < nargs; i++)
        W32(sp + 4u + 4u * (uint32_t)i, args[i]);
    g_esp = sp;
    fn();
    if (g_esp != (callee_pops ? STACK_TOP : sp + 4u)) {
        fprintf(stderr, "esp came back as 0x%08X\n", g_esp);
        exit(1);
    }
}

int main(int argc, char **argv)
{
    uint8_t *mem;
    int which, i;
    long iters, k;
    unsigned sum = 0;
    double t0, t1;

    if (argc < 3) {
        fprintf(stderr, "usage: bench <0..4> <iterations>\n");
        return 2;
    }
    which = atoi(argv[1]);
    iters = atol(argv[2]);
    mem = (uint8_t *)calloc(16u << 20, 1);
    if (!mem) return 1;
    g_xbox_mem_offset = (ptrdiff_t)mem;

    /* clear: [0x4E7E34] -> pointer to the buffer pointer, [0x4E7E38] = bytes */
    W32(0x4E7E34, 0x100000); W32(0x100000, 0x200000); W32(0x4E7E38, 4096 + 3);
    memset(GUEST(0x200000), 0xA5, 4096 + 3);
    /* matmul: A at 0x300000, B at 0x300040, out at 0x300080 */
    for (i = 0; i < 16; i++) {
        WF(0x300000 + 4 * i, i * 0.5f + 1);
        WF(0x300040 + 4 * i, 2.0f - i * 0.25f);
    }
    /* keysearch: 64 keys of {t, v0, v1, v2} at 0x310000 */
    for (i = 0; i < 64; i++) {
        WF(0x310000 + 16 * i, i);
        WF(0x310004 + 16 * i, i * 2);
        WF(0x310008 + 16 * i, i * 3);
        WF(0x31000C + 16 * i, i * 4);
    }
    /* calls: 100 triples at 0x330000, accumulator at 0x340000 */
    for (i = 0; i < 300; i++) W32(0x330000 + 4 * i, i * 7 + 1);
    /* x87sum: 256 float pairs at 0x350000 */
    for (i = 0; i < 512; i++) WF(0x350000 + 4 * i, (i % 13) * 0.25f);
    g_fp_top = 0;

    t0 = now();
    for (k = 0; k < iters; k++) {
        if (which == 0) {
            /* Poison both ends of each region so a variant that clears too
             * little, or nothing, changes the checksum. */
            W32(0x200000, k + 1); W32(0x200000 + 2048, k + 2);
            W32(0x200000 + 4095, k + 3); W32(0x4E7F00 + 124, k + 4);
            W32(0x200000 + 4099, 0x5A5A5A5Au);   /* one past the end */
            guest_call(sub_000A5230, 0, NULL, 1);
            sum += R32(0x200000) + R32(0x200000 + 2048) + R32(0x200000 + 4095)
                 + R32(0x4E7F00 + 124) + R32(0x200000 + 4099);
        } else if (which == 1) {
            uint32_t a[3] = {0x300080, 0x300000, 0x300040};
            guest_call(sub_000F7487, 3, a, 1);
            sum += R32(0x300080 + 4 * (k & 15));
        } else if (which == 2) {
            float t = (float)((k * 7) % 6300) / 100.0f;
            uint32_t tb, a[4];
            memcpy(&tb, &t, 4);
            a[0] = 0x310000; a[1] = 64; a[2] = tb; a[3] = 0x320000;
            guest_call(sub_00011000, 4, a, 1);
            sum += R32(0x320000) + R32(0x320008);
        } else if (which == 3) {
            uint32_t a[3] = {0x330000, 0x340000, 100};
            W32(0x340000, 0);
            guest_call(sub_00090200, 3, a, 0);
            sum += R32(0x340000);
        } else {
            uint32_t a[3] = {0x350000, 256, 0x360000};
            guest_call(sub_00090000, 3, a, 0);
            sum += R32(0x360000) + (uint32_t)g_fp_top;
        }
    }
    t1 = now();
    printf("%.2f %u\n", (t1 - t0) * 1e9 / (double)iters, sum);
    free(mem);
    return 0;
}
