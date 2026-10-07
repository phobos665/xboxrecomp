/*
 * guest_memory - the POSIX memory backend keeps Windows' semantics.
 *
 * The layout code was written against Windows and depends on three things
 * POSIX does not give for free: a view placed at a fixed address either lands
 * there or fails (it never replaces what is mapped), views of one object
 * alias, and protection works on 4 KB pages. This checks all three inside a
 * guest arena like the one xbox_MemoryLayoutInit reserves, including on a
 * 16 KB-page host, where a 4 KB protection change has to leave the other
 * 12 KB of the host page usable -- by the fault handler, through the
 * backdoor alias.
 *
 * Needs no game files and no runtime beyond src/platform.
 */
#include "win32_compat.h"

#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(name, cond) \
    do { if (cond) {} else { printf("FAIL: %s (line %d)\n", name, __LINE__); failures++; } } while (0)

/* Does touching p fault? */
static sigjmp_buf s_jmp;
static volatile sig_atomic_t s_faulted;

static void on_fault(int sig, siginfo_t *si, void *uc)
{
    (void)sig; (void)si; (void)uc;
    s_faulted = 1;
    siglongjmp(s_jmp, 1);
}

static int faults_on_write(volatile uint32_t *p)
{
    s_faulted = 0;
    if (!sigsetjmp(s_jmp, 1))
        *p = 0x5A5A5A5Au;
    return s_faulted;
}

static int faults_on_read(volatile uint32_t *p)
{
    s_faulted = 0;
    if (!sigsetjmp(s_jmp, 1)) {
        volatile uint32_t v = *p;
        (void)v;
    }
    return s_faulted;
}

#define MB (1024u * 1024u)

int main(void)
{
    struct sigaction sa;
    size_t page = w32_host_page_size();
    uint8_t *base;
    HANDLE ram;
    int m;

    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = on_fault;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);

    printf("guest_memory: host page %zu bytes\n", page);

    /* The arena: 4 GB of guest span plus a guard, aligned to 4 GB. */
    base = (uint8_t *)w32_reserve_arena(0x100000000ull + 0x10000, 0x100000000ull);
    CHECK("arena reserved", base != NULL);
    if (!base) return 1;
    CHECK("arena 4 GB aligned", ((uintptr_t)base & 0xFFFFFFFFull) == 0);
    CHECK("second reserve refused", w32_reserve_arena(page, page) == NULL);
    CHECK("arena is inaccessible until placed", faults_on_read((uint32_t *)(base + 0x1000)));

    /* RAM: one object, a base view and mirrors. */
    ram = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, 64 * MB, NULL);
    CHECK("RAM object", ram != NULL);
    CHECK("base view placed",
          MapViewOfFileEx(ram, FILE_MAP_ALL_ACCESS, 0, 0, 64 * MB, base) == base);
    CHECK("second view at the same address refused",
          MapViewOfFileEx(ram, FILE_MAP_ALL_ACCESS, 0, 0, 64 * MB, base) == NULL);
    CHECK("overlapping view refused",
          MapViewOfFileEx(ram, FILE_MAP_ALL_ACCESS, 0, 0, 64 * MB, base + 32 * MB) == NULL);
    for (m = 1; m <= 28; m++)
        CHECK("mirror placed",
              MapViewOfFileEx(ram, FILE_MAP_ALL_ACCESS, 0, 0, 64 * MB,
                              base + (size_t)m * 64 * MB) == base + (size_t)m * 64 * MB);
    *(volatile uint32_t *)(base + 0x00070000) = 0x11223344u;
    CHECK("mirror 1 aliases", *(volatile uint32_t *)(base + 0x04070000) == 0x11223344u);
    CHECK("mirror 28 aliases", *(volatile uint32_t *)(base + 0x70070000) == 0x11223344u);
    *(volatile uint32_t *)(base + 0x20000448) = 0xCAFEF00Du;
    CHECK("write through a mirror lands in RAM", *(volatile uint32_t *)(base + 0x448) == 0xCAFEF00Du);

    /* Unplacing leaves the range reserved, and it can be placed again. */
    CHECK("unmap mirror 3", UnmapViewOfFile(base + 3 * 64 * MB));
    CHECK("unmapped mirror faults", faults_on_read((uint32_t *)(base + 3 * 64 * MB)));
    CHECK("mirror 3 placed again",
          MapViewOfFileEx(ram, FILE_MAP_ALL_ACCESS, 0, 0, 64 * MB, base + 3 * 64 * MB)
              == base + 3 * 64 * MB);
    CHECK("re-placed mirror aliases", *(volatile uint32_t *)(base + 0x0C000448) == 0xCAFEF00Du);

    /* A separate contiguous object, and a second view of it (the tiled
     * aperture), as the layout does. */
    {
        HANDLE contig = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE,
                                           0, 64 * MB, NULL);
        uint8_t *c = base + 0x80000000u, *t = base + 0xF0000000u;
        CHECK("contiguous placed",
              MapViewOfFileEx(contig, FILE_MAP_ALL_ACCESS, 0, 0, 64 * MB, c) == c);
        CHECK("tiled placed",
              MapViewOfFileEx(contig, FILE_MAP_ALL_ACCESS, 0, 0, 64 * MB, t) == t);
        *(volatile uint32_t *)(t + 0x1000) = 0xA5C30F17u;
        CHECK("tiled aliases contiguous", *(volatile uint32_t *)(c + 0x1000) == 0xA5C30F17u);
        CHECK("contiguous is not RAM", *(volatile uint32_t *)(base + 0x1000) != 0xA5C30F17u);
        CHECK("a RAM mirror over the contiguous window is refused",
              MapViewOfFileEx(ram, FILE_MAP_ALL_ACCESS, 0, 0, 64 * MB, c) == NULL);
        /* Closing the handle must not take the views with it. */
        CloseHandle(contig);
        CHECK("views outlive the handle", *(volatile uint32_t *)(c + 0x1000) == 0xA5C30F17u);
        CHECK("backdoor outlives the handle",
              w32_backdoor(t + 0x1000) && *(volatile uint32_t *)w32_backdoor(t + 0x1000) == 0xA5C30F17u);
    }

    /* A device aperture by VirtualAlloc, and 4 KB protection inside it. */
    {
        uint8_t *nv = base + 0xFD000000u;
        uint8_t *pcrtc = nv + 0x600000;
        MEMORY_BASIC_INFORMATION mbi;
        DWORD old = 0;

        CHECK("aperture allocated",
              VirtualAlloc(nv, 16 * MB, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE) == nv);
        CHECK("overlapping allocation refused",
              VirtualAlloc(nv + MB, MB, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE) == NULL);
        CHECK("aperture zeroed", *(volatile uint32_t *)(nv + 0x1804) == 0);

        CHECK("protect one guest page read-only",
              VirtualProtect(pcrtc, 4096, PAGE_READONLY, &old));
        CHECK("old protection reported", old == PAGE_READWRITE);
        CHECK("side table: trapped page", w32_page_protection(pcrtc) == PAGE_READONLY);
        CHECK("side table: neighbour untouched",
              w32_page_protection(pcrtc + 0x1000) == PAGE_READWRITE);
        CHECK("write to the trapped page faults", faults_on_write((uint32_t *)(pcrtc + 0x100)));
        CHECK("read of the trapped page does not", !faults_on_read((uint32_t *)(pcrtc + 0x100)));
        if (page > 4096) {
            /* The host page is shared: a write to the neighbour faults too,
             * and it is the fault handler that must complete it. */
            CHECK("neighbour write faults on a big-page host",
                  faults_on_write((uint32_t *)(pcrtc + 0x1000)));
        } else {
            CHECK("neighbour write is plain on a 4 KB host",
                  !faults_on_write((uint32_t *)(pcrtc + 0x1000)));
        }
        /* Through the backdoor the store lands and the guest view sees it. */
        *(volatile uint32_t *)w32_backdoor(pcrtc + 0x1004) = 0x600DF00Du;
        CHECK("backdoor store visible", *(volatile uint32_t *)(pcrtc + 0x1004) == 0x600DF00Du);
        *(volatile uint32_t *)w32_backdoor(pcrtc + 0x100) = 1;
        CHECK("backdoor writes a read-only page", *(volatile uint32_t *)(pcrtc + 0x100) == 1);

        CHECK("query the trapped page", VirtualQuery(pcrtc + 0x10, &mbi, sizeof(mbi)) == sizeof(mbi));
        CHECK("query: base is the 4 KB page", mbi.BaseAddress == pcrtc);
        CHECK("query: allocation base", mbi.AllocationBase == nv);
        CHECK("query: one page of read-only", mbi.RegionSize == 4096 && mbi.Protect == PAGE_READONLY);
        VirtualQuery(nv, &mbi, sizeof(mbi));
        CHECK("query: run up to the trapped page", mbi.RegionSize == 0x600000 && mbi.Protect == PAGE_READWRITE);
        VirtualQuery(base + 0xFE000000u, &mbi, sizeof(mbi));
        CHECK("query: unplaced is free", mbi.State == 0x10000u && mbi.AllocationBase == NULL);

        CHECK("unprotect", VirtualProtect(pcrtc, 4096, PAGE_READWRITE, &old) && old == PAGE_READONLY);
        CHECK("neighbour plain again", !faults_on_write((uint32_t *)(pcrtc + 0x1000)));
        CHECK("trapped page plain again", !faults_on_write((uint32_t *)(pcrtc + 0x100)));

        /* Protection that is not page aligned covers the pages it touches,
         * as on Windows: the thunk-table patch protects 4 * n bytes. */
        CHECK("unaligned protect", VirtualProtect(base + 0x12344, 8, PAGE_READWRITE, &old));
        CHECK("unaligned protect: old", old == PAGE_READWRITE);

        /* Page zero, as RECOMP_TRAP_NULL does, with a TIB above it. */
        CHECK("trap guest page 0", VirtualProtect(base, 4096, PAGE_NOACCESS, &old));
        CHECK("null read faults", faults_on_read((uint32_t *)(base + 8)));
        CHECK("a TIB at 0x4000 is clear of the trap", !faults_on_write((uint32_t *)(base + 0x4000)));
        CHECK("guest 0x1000 is still guest-accessible",
              w32_page_protection(base + 0x1000) == PAGE_READWRITE);
        VirtualProtect(base, 4096, PAGE_READWRITE, &old);

        CHECK("protect outside any placement fails",
              !VirtualProtect(base + 0xFE000000u, 4096, PAGE_READONLY, &old));
        CHECK("release aperture", VirtualFree(nv, 0, MEM_RELEASE));
        CHECK("released aperture faults", faults_on_read((uint32_t *)nv));
        CHECK("released side table cleared", w32_page_protection(nv) == 0);
    }

    /* Outside the arena: fixed requests must not replace, and MEM_RELEASE
     * with size 0 has to free what VirtualAlloc made. */
    {
        MEMORY_BASIC_INFORMATION mbi;
        uint8_t *p = (uint8_t *)VirtualAlloc(NULL, 3 * page, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        CHECK("host allocation", p != NULL);
        p[0] = 7;
        CHECK("fixed request over it refused",
              VirtualAlloc(p, page, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE) == NULL);
        CHECK("still intact", p[0] == 7);
        VirtualQuery(p + page + 5, &mbi, sizeof(mbi));
        CHECK("query names the allocation base", mbi.AllocationBase == p);
        CHECK("release with size 0", VirtualFree(p, 0, MEM_RELEASE));
        VirtualQuery(p, &mbi, sizeof(mbi));
        CHECK("released: no longer ours", mbi.AllocationBase == NULL);
        CHECK("a malloc'd pointer has no allocation base",
              (VirtualQuery(&mbi, &mbi, sizeof(mbi)), mbi.AllocationBase == NULL));
    }

    if (failures) {
        printf("guest_memory: %d FAILURE(S)\n", failures);
        return 1;
    }
    printf("guest_memory: ALL PASS\n");
    return 0;
}
