/*
 * guest_faults - trapped guest pages behave as the runtime expects, through
 * the real fault path: recomp_fault_install's signal handler, a route built
 * from fault_emulate.h's pieces in xbox_fault_route's order, the guest arena
 * and the host's decoder.
 *
 *   - a device range (PAGE_NOACCESS, like the APU's registers) answers reads
 *     from its model and sees every write, with the faulting instruction
 *     completed and the thread resumed after it;
 *   - a write-only trap page (PAGE_READONLY, like the AC'97 page) reads as
 *     plain memory and routes writes;
 *   - on a 16 KB-page host, the other 12 KB around a 4 KB trap is plain
 *     guest memory: reads and writes land, from C compiled like lifted code;
 *   - a genuine bad access (guest page zero under RECOMP_TRAP_NULL) is not
 *     swallowed: it reaches the crash callback.
 *
 * POSIX only; needs no game files. Links the platform library alone.
 */
#include "win32_compat.h"
#include "recomp_fault.h"
#include "fault_emulate.h"

#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(name, cond) \
    do { if (cond) {} else { printf("FAIL: %s (line %d)\n", name, __LINE__); failures++; } } while (0)

#define MB (1024u * 1024u)
#define DEV_VA   0xFE800000u      /* a device: reads and writes routed */
#define WO_VA    0xFEC00000u      /* a write-only trap page */

static uintptr_t s_base;

/* Volatile: the fault handler changes these behind the compiler's back, and
 * without it GCC -O2 reads them before the store that faults (a volatile
 * store orders only against other volatile accesses). */
static volatile struct { uint32_t last_off; uint64_t last_val; int reads, writes; } dev;

static uint64_t dev_rd(void *d, uint32_t va, int size)
{
    (void)d; (void)size;
    dev.reads++;
    dev.last_off = va - DEV_VA;
    return 0xC0DE0000u | (va & 0xFFFFu);
}

static void dev_wr(void *d, uint32_t va, uint64_t v, int size)
{
    (void)d; (void)size;
    dev.writes++;
    dev.last_off = va - DEV_VA;
    dev.last_val = v;
}

static int dev_fault(recomp_fault *f, uint32_t va)
{
    (void)va;
    return recomp_fault_emulate(f, s_base, NULL, dev_rd, dev_wr);
}

static volatile int wo_writes;
static void wo_wr(void *d, uint32_t va, uint64_t v, int size)
{
    wo_writes++;
    recomp_guest_write(d, va, v ^ 0xFF, size);     /* the "model": invert a byte */
}

static int wo_fault(recomp_fault *f, uint32_t va)
{
    (void)va;
    return recomp_fault_emulate(f, s_base, NULL, recomp_guest_read, wo_wr);
}

/* xbox_fault_route's order, without the kernel. */
static int route(recomp_fault *f)
{
    if (f->kind != RECOMP_FAULT_ACCESS || f->host_addr < s_base
        || f->host_addr - s_base > 0xFFFFFFFFu)
        return 0;
    if (recomp_fault_dispatch_ranges(f, (uint32_t)(f->host_addr - s_base)))
        return 1;
    return recomp_fault_passthrough(f);
}

static sigjmp_buf s_crash_jmp;
static volatile int s_crashes;
static volatile uintptr_t s_crash_addr;

static void crash(const recomp_fault *f)
{
    s_crashes++;
    s_crash_addr = f->host_addr;
    siglongjmp(s_crash_jmp, 1);       /* the test recovers; a title would die */
}

#define G8(a)  (*(volatile uint8_t  *)(s_base + (uint32_t)(a)))
#define G16(a) (*(volatile uint16_t *)(s_base + (uint32_t)(a)))
#define G32(a) (*(volatile uint32_t *)(s_base + (uint32_t)(a)))
#define G64(a) (*(volatile uint64_t *)(s_base + (uint32_t)(a)))

int main(void)
{
    HANDLE ram;
    DWORD old;
    size_t page = w32_host_page_size();

    printf("guest_faults: host page %zu bytes\n", page);
    s_base = (uintptr_t)w32_reserve_arena(0x100000000ull + 0x10000u, 0x100000000ull);
    CHECK("arena", s_base != 0);
    if (!s_base) return 1;
    ram = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, 64 * MB, NULL);
    CHECK("RAM", MapViewOfFileEx(ram, FILE_MAP_ALL_ACCESS, 0, 0, 64 * MB, (void *)s_base)
                 == (void *)s_base);
    CHECK("MCPX aperture", VirtualAlloc((void *)(s_base + 0xFE800000u), 8 * MB,
                                        MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE) != NULL);
    recomp_fault_set_guest_base(s_base);

    CHECK("device range", recomp_fault_add_range(DEV_VA, DEV_VA + 0x1000, 0, dev_fault, "dev") == 0);
    CHECK("write-only range", recomp_fault_add_range(WO_VA, WO_VA + 0x1000, 1, wo_fault, "wo") == 0);
    G32(WO_VA + 0x130) = 0x100;                       /* codec ready, before the trap */
    CHECK("trap device", VirtualProtect((void *)(s_base + DEV_VA), 0x1000, PAGE_NOACCESS, &old));
    CHECK("trap wo page", VirtualProtect((void *)(s_base + WO_VA), 0x1000, PAGE_READONLY, &old));
    CHECK("trap page 0", VirtualProtect((void *)s_base, 0x1000, PAGE_NOACCESS, &old));

    recomp_fault_install(route, crash);

    /* The device: reads answered by the model, writes seen. */
    {
        uint32_t v = G32(DEV_VA + 0x100);
        CHECK("device read value", v == (0xC0DE0000u | 0x0100u));
        CHECK("device read offset", dev.last_off == 0x100 && dev.reads == 1);
        G16(DEV_VA + 0x204) = 0xBEEF;
        CHECK("device write", dev.writes == 1 && dev.last_off == 0x204 && dev.last_val == 0xBEEF);
        G8(DEV_VA + 0x3) = 0x7F;
        CHECK("device byte write", dev.writes == 2 && dev.last_val == 0x7F);
    }

    /* The write-only page: reads plain, writes through the model. */
    CHECK("wo read is plain", G32(WO_VA + 0x130) == 0x100 && wo_writes == 0);
    G8(WO_VA + 0x11B) = 0x02;
    CHECK("wo write routed", wo_writes == 1 && G8(WO_VA + 0x11B) == (0x02 ^ 0xFF));

    /* Collateral: the rest of the host page. Only interesting on a big-page
     * host, harmless elsewhere. */
    {
        uint32_t a = WO_VA + 0x1000, i;
        uint64_t sum = 0;
        for (i = 0; i < 64; i++) {
            G32(a + i * 16) = i * 3u;
            G64(a + i * 16 + 8) = (uint64_t)i << 33;
        }
        for (i = 0; i < 64; i++)
            sum += G32(a + i * 16) + (G64(a + i * 16 + 8) >> 33);
        CHECK("collateral round trip", sum == (uint64_t)(63 * 64 / 2) * 4);
        CHECK("collateral did not reach the model", wo_writes == 1);
        CHECK("guest 0x1000 usable beside the null trap", (G32(0x1000) = 0x1234, G32(0x1000) == 0x1234));
        memcpy((void *)(s_base + DEV_VA + 0x1000), "collateral memcpy", 18);
        CHECK("collateral memcpy", memcmp((void *)(s_base + DEV_VA + 0x1000),
                                         "collateral memcpy", 18) == 0);
        CHECK("memcpy beside the device did not reach it", dev.writes == 2);
    }

    /* Atomics on collateral memory are real atomics through the backdoor:
     * LSE add and CAS from C, and an LDXR/STXR loop by hand -- what the
     * lifter's lowering of `lock` instructions compiles to. */
    {
        uint32_t a = WO_VA + 0x2000;
        volatile uint32_t *p = (volatile uint32_t *)(s_base + a);
        uint32_t expect = 7, old;
        int i;

        *p = 5;
        for (i = 0; i < 100; i++)
            __atomic_fetch_add(p, 1, __ATOMIC_SEQ_CST);
        CHECK("LSE add on collateral", *p == 105);
        expect = 105;
        CHECK("CAS succeeds", __atomic_compare_exchange_n(p, &expect, 200, 0,
                                                          __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)
                              && *p == 200);
        expect = 105;
        CHECK("CAS fails and reports the value",
              !__atomic_compare_exchange_n(p, &expect, 300, 0,
                                           __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)
              && expect == 200 && *p == 200);
#if defined(__aarch64__)
        for (i = 0; i < 10; i++) {
            uint32_t status;
            __asm__ volatile(
                "1: ldxr %w0, [%2]\n"
                "   add  %w0, %w0, #3\n"
                "   stxr %w1, %w0, [%2]\n"
                "   cbnz %w1, 1b\n"
                : "=&r"(old), "=&r"(status) : "r"(p) : "memory");
        }
        CHECK("LDXR/STXR loop on collateral", *p == 230);
        /* Beside the null trap the host page is no-access, so the LDXR
         * faults too and the STXR becomes a compare-and-swap against what
         * it saw. */
        p = (volatile uint32_t *)(s_base + 0x2000);
        *p = 1;
        for (i = 0; i < 10; i++) {
            uint32_t status;
            __asm__ volatile(
                "1: ldaxr %w0, [%2]\n"
                "   add   %w0, %w0, #2\n"
                "   stlxr %w1, %w0, [%2]\n"
                "   cbnz  %w1, 1b\n"
                : "=&r"(old), "=&r"(status) : "r"(p) : "memory");
        }
        CHECK("LDAXR/STLXR loop beside the null trap", *p == 21);
#else
        (void)old;
#endif
    }

    /* A real bad access still crashes. */
    if (!sigsetjmp(s_crash_jmp, 1)) {
        volatile uint32_t v = G32(0x8);
        (void)v;
    }
    CHECK("null read reaches the crash report", s_crashes == 1 && s_crash_addr == s_base + 8);

    if (failures) {
        printf("guest_faults: %d FAILURE(S)\n", failures);
        return 1;
    }
    printf("guest_faults: ALL PASS\n");
    return 0;
}
