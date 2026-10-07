/*
 * memory_layout - the real xbox_MemoryLayoutInit, on a synthetic XBE, with
 * every alias and aperture checked afterwards.
 *
 *   memory_layout_test 64     a retail-shaped image: a 64 MB map
 *   memory_layout_test 128    40 MB of demand-loaded sections: a 128 MB map
 *                             (BLiNX's shape)
 *
 * One map per process: the layout initialises once. Checked for each:
 * RAM's base view and every mirror are one memory, mirrors stop below
 * 0x80000000 (28 of 64 MB, 15 of 128 MB) and nothing past the last one is
 * RAM; the contiguous window is separate storage and the tiled aperture is
 * a view of it; the NV2A, MCPX and flash apertures exist, with the trap pages
 * protected as the switches ask (4 KB each, whatever the host page); the
 * fake kernel PE header; the main TIB at 0x4000 and page zero trapped under
 * RECOMP_TRAP_NULL without taking the TIB with it.
 *
 * Inspects trapped pages with VirtualQuery, never by touching them: there is
 * no fault route in this process. Needs no game files.
 */
#include "kernel.h"
#include "xbox_memory_layout.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>   /* _exit */

void *recomp_lookup(ULONG address) { (void)address; return NULL; }
void *recomp_lookup_manual(ULONG address) { (void)address; return NULL; }

extern ptrdiff_t g_xbox_mem_offset;

static int failures;
#define CHECK(name, cond) \
    do { if (cond) {} else { printf("FAIL: %s (line %d)\n", name, __LINE__); failures++; } } while (0)

#define MB (1024u * 1024u)

static uint8_t *G(uint32_t va) { return (uint8_t *)g_xbox_mem_offset + va; }
static volatile uint32_t *G32(uint32_t va) { return (volatile uint32_t *)G(va); }

static DWORD prot_of(uint32_t va)
{
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(G(va), &mbi, sizeof(mbi)))
        return 0xFFFFFFFFu;
    return mbi.State == MEM_COMMIT ? (mbi.Protect & 0xFF) : 0;   /* 0: nothing there */
}

static void put32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }

/* A minimal XBE: header, certificate, one code section, and optionally one
 * big demand-loaded data section. */
static uint8_t *make_xbe(int demand_mb, size_t *size)
{
    size_t n = 0x3000;
    uint8_t *x = (uint8_t *)calloc(1, n);
    int sections = demand_mb ? 2 : 1;

    memcpy(x, "XBEH", 4);
    put32(x + 0x104, 0x00010000);                 /* base address */
    put32(x + 0x108, 0x1000);                     /* size of headers */
    put32(x + 0x10C, 0x00100000 + (uint32_t)demand_mb * MB);   /* size of image */
    put32(x + 0x118, 0x00010200);                 /* certificate */
    put32(x + 0x11C, (uint32_t)sections);
    put32(x + 0x120, 0x00010300);                 /* section headers */
    put32(x + 0x12C, 0);                          /* no TLS */
    put32(x + 0x158, 0x00011000u ^ 0x5B6D40B6u);  /* thunks (retail key): empty */
    memcpy(x + 0x400, ".text\0\0\0MDL\0\0\0\0\0", 16);

    /* section 0: .text, preload | executable, raw at 0x1000 */
    put32(x + 0x300 + 0x00, 0x00000006);
    put32(x + 0x300 + 0x04, 0x00011000);
    put32(x + 0x300 + 0x08, 0x1000);
    put32(x + 0x300 + 0x0C, 0x1000);
    put32(x + 0x300 + 0x10, 0x1000);
    put32(x + 0x300 + 0x14, 0x00010400);
    memset(x + 0x1004, 0xC3, 0x100);
    if (demand_mb) {
        /* section 1: MDL, not preloaded, demand_mb of BSS */
        put32(x + 0x338 + 0x00, 0x00000000);
        put32(x + 0x338 + 0x04, 0x00012000);
        put32(x + 0x338 + 0x08, (uint32_t)demand_mb * MB);
        put32(x + 0x338 + 0x0C, 0);
        put32(x + 0x338 + 0x10, 0);
        put32(x + 0x338 + 0x14, 0x00010408);
    }
    *size = n;
    return x;
}

int main(int argc, char **argv)
{
    int map_mb = argc > 1 ? atoi(argv[1]) : 64;
    uint32_t map = (uint32_t)map_mb * MB;
    int expect_mirrors = (int)(0x80000000u / map) - 1;
    size_t xbe_size;
    uint8_t *xbe;
    int m;

    if (map_mb != 64 && map_mb != 128) {
        printf("usage: memory_layout_test 64|128\n");
        return 2;
    }
    /* The switches a title runs with by default, and the null trap. */
    setenv("RECOMP_TRAP_NULL", "1", 1);
    setenv("RECOMP_VBLANK", "1", 1);
    setenv("RECOMP_AC97_READY", "1", 1);

    xbe = make_xbe(map_mb == 128 ? 40 : 0, &xbe_size);
    CHECK("layout init", xbox_MemoryLayoutInit(xbe, xbe_size));
    if (failures) return 1;
    printf("memory_layout %d MB: guest base %p, mapped %zu MB\n", map_mb,
           (void *)g_xbox_mem_offset, xbox_GetMappedSize() / MB);

    CHECK("mapped size", xbox_GetMappedSize() == map);
#if !defined(_WIN32)
    CHECK("guest base 4 GB aligned", ((uintptr_t)g_xbox_mem_offset & 0xFFFFFFFFull) == 0);
#endif

    /* The image is where the title expects it. */
    CHECK("XBE header at 0x10000", memcmp(G(0x10000), xbe, 0x200) == 0);
    CHECK(".text at 0x11000", *G(0x11004) == 0xC3);

    /* RAM and its mirrors are one memory. */
    *G32(0x00070000) = 0x5EED0001u;
    for (m = 1; m <= expect_mirrors; m++) {
        uint32_t at = (uint32_t)m * map + 0x00070000u;
        if (*G32(at) != 0x5EED0001u) {
            printf("  mirror %d at 0x%08X does not alias RAM\n", m, at);
            failures++;
        }
    }
    *G32((uint32_t)expect_mirrors * map + 0x123450u) = 0x5EED0002u;
    CHECK("write through the last mirror lands in RAM", *G32(0x123450u) == 0x5EED0002u);
    if (map_mb == 64)
        CHECK("nothing past the 28th mirror", prot_of(0x74000000u) == 0);
    CHECK("the last mirror ends at or below 0x80000000",
          (uint64_t)(expect_mirrors + 1) * map <= 0x80000000ull);

    /* The contiguous window: its own storage, with the tiled aperture over
     * it, and nothing of RAM. */
    CHECK("contiguous window is RW", prot_of(0x80000000u) == PAGE_READWRITE);
    CHECK("contiguous is not a RAM mirror", *G32(0x80070000u) != 0x5EED0001u);
    *G32(0xF0001000u) = 0xA5C30F17u;
    CHECK("tiled aliases contiguous", *G32(0x80001000u) == 0xA5C30F17u);
    CHECK("tiled is not RAM", *G32(0x00001000u) != 0xA5C30F17u);
    CHECK("fake kernel PE header", *G32(0x8001003Cu) == 0x80u);

    /* The device apertures, and the 4 KB trap pages in them. */
    CHECK("NV2A aperture RW", prot_of(0xFD000000u) == PAGE_READWRITE);
    CHECK("PCRTC page write-trapped", prot_of(0xFD600000u) == PAGE_READONLY);
    CHECK("the page after PCRTC is RW", prot_of(0xFD601000u) == PAGE_READWRITE);
    CHECK("APU registers trapped", prot_of(0xFE800000u) == PAGE_NOACCESS
                                && prot_of(0xFE82F000u) == PAGE_NOACCESS);
    CHECK("APU DSP memory plain", prot_of(0xFE830000u) == PAGE_READWRITE);
    CHECK("AC97 page write-trapped", prot_of(0xFEC00000u) == PAGE_READONLY);
    CHECK("AC97 codec ready", (*G32(0xFEC00130u) & 0x100u) != 0);
    CHECK("flash aperture RW", prot_of(0xFF000000u) == PAGE_READWRITE);

    /* Page zero under RECOMP_TRAP_NULL, and the TIB clear of it. */
    CHECK("guest page 0 trapped", prot_of(0x0u) == PAGE_NOACCESS);
    CHECK("guest 0x1000 untouched by the trap", prot_of(0x1000u) == PAGE_READWRITE);
    CHECK("TIB at 0x4000: SEH end of chain", *G32(0x4000u) == 0xFFFFFFFFu);
    CHECK("TIB at 0x4000: self pointer", *G32(0x4018u) == 0x4000u);

    if (failures) {
        printf("memory_layout %d MB: %d FAILURE(S)\n", map_mb, failures);
        return 1;
    }
    printf("memory_layout %d MB: ALL PASS (%d mirrors)\n", map_mb, expect_mirrors);
    fflush(stdout);
    _exit(0);   /* the layout's ack thread is still running; no teardown */
}
