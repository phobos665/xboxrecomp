# Memory Layout Reproduction

How to map Xbox memory at its original virtual addresses on Windows so that recompiled code with hardcoded addresses works unmodified.

## The Problem

Xbox games are compiled with hardcoded absolute addresses. The Burnout 3 binary contains thousands of instructions like:

```asm
mov eax, [0x004D532C]    ; read global variable
cmp dword ptr [0x4A1C74], 0  ; check button state
mov [0x5FFD08], ecx       ; write boost meter
```

After recompilation to C, these become:

```c
eax = MEM32(0x004D532C);
if (CMP_EQ(MEM32(0x4A1C74), 0)) goto loc_xyz;
MEM32(0x5FFD08) = ecx;
```

For this to work, reading address 0x004D532C must return the actual game data that was originally at that address. The entire Xbox memory map must be reproduced at the correct virtual addresses.

## Xbox Memory Map

The Xbox has 64 MB of unified RAM shared between CPU and GPU. Key regions:

| Address Range | Content | Size |
|---------------|---------|------|
| 0x00000000-0x000000FF | KPCR / TIB (thread info block) | 256 B |
| 0x00010000-0x00010FFF | XBE image header | 4 KB |
| 0x00011000-0x002CCFFF | .text (game code) | 2.73 MB |
| 0x002CC200-0x0036B7BF | XDK library code (D3D, DSOUND, XMV, etc.) | ~600 KB |
| 0x0036B7C0-0x003B2354 | .rdata (constants, strings, vtables) | 280 KB |
| 0x003B2360-0x0076EFFF | .data + BSS (globals, zero-initialized data) | 3.9 MB |
| 0x00780000-0x00F7FFFF | Stack (8 MB, grows downward) | 8 MB |
| 0x00F80000-0x03FFFFFF | Dynamic heap (bump allocator) | ~49 MB |
| 0x80010000+ | Xbox kernel PE header (fake, 1 page) | 4 KB |
| 0xFD000000+ | NV2A GPU registers (on-demand allocation) | Variable |
| 0xFE000000+ | Kernel function thunks (synthetic VAs) | ~600 B |

Total mapped: 64 MB contiguous at the base, plus special regions.

## Why CreateFileMapping, Not VirtualAlloc

The Xbox memory controller uses a 26-bit address bus. All addresses wrap modulo 64 MB:

```
Address 0x04070000 == Address 0x00070000  (both access same physical byte)
Address 0x20000448 == Address 0x00000448
```

The RenderWare engine exploits this. Its memory walker crosses 64 MB and reads mirrored data for an extended walk covering 256+ MB of virtual addresses. Game initialization code also writes large data structures past 64 MB that on real hardware wrap into physical RAM.

**VirtualAlloc cannot do this.** VirtualAlloc gives you distinct physical pages at each virtual address. Writing to 0x04070000 does NOT update the data at 0x00070000. We wasted days debugging this before switching to file mappings.

**CreateFileMapping + MapViewOfFileEx** creates true aliases. Multiple virtual address ranges can map to the same physical pages:

```c
// Create a page-file-backed mapping for 64 MB
HANDLE mapping = CreateFileMappingA(
    INVALID_HANDLE_VALUE,  // page file backed
    NULL,                  // default security
    PAGE_READWRITE,
    0, 64 * 1024 * 1024,  // 64 MB
    NULL                   // unnamed
);

// Map the base view at the desired Xbox address
void *base = MapViewOfFileEx(
    mapping,
    FILE_MAP_ALL_ACCESS,
    0, 0,
    64 * 1024 * 1024,
    (LPVOID)0x00000000     // desired base address
);

// Map mirror views at 64 MB intervals
for (int m = 0; m < 28; m++) {
    uintptr_t mirror_addr = (uintptr_t)base + (m + 1) * 64 * 1024 * 1024;
    void *mirror = MapViewOfFileEx(
        mapping,
        FILE_MAP_ALL_ACCESS,
        0, 0,
        64 * 1024 * 1024,
        (LPVOID)mirror_addr
    );
    // Now writes to mirror_addr + X are visible at base + X
}
```

With 28 mirror views covering 1.75 GB of address space, the RenderWare memory walker can traverse the full range it expects.

## Address Translation: The XBOX_PTR Macro

All memory access goes through a single macro:

```c
#define XBOX_PTR(addr) ((uintptr_t)(uint32_t)(addr) + g_xbox_mem_offset)

#define MEM8(addr)   (*(volatile uint8_t  *)XBOX_PTR(addr))
#define MEM16(addr)  (*(volatile uint16_t *)XBOX_PTR(addr))
#define MEM32(addr)  (*(volatile uint32_t *)XBOX_PTR(addr))
#define MEMF(addr)   (*(volatile float    *)XBOX_PTR(addr))
```

### Why the uint32_t Cast is Essential

The `(uint32_t)` cast in XBOX_PTR is critical. On a 64-bit Windows build, `uintptr_t` is 64 bits. Without the cast:

```c
// WRONG: if addr is the result of (0xFFFFFFFF + 1), it becomes 0x100000000
// on 64-bit, this is 4 GB past our mapping -> access violation
#define XBOX_PTR_BAD(addr) ((uintptr_t)(addr) + g_xbox_mem_offset)
```

Xbox addresses are 32-bit and arithmetic in recompiled code can overflow. The uint32_t cast truncates to 32 bits first, matching Xbox hardware behavior where addresses wrap at 4 GB:

```c
// CORRECT: overflow wraps to 32 bits, then extends to 64-bit for the add
#define XBOX_PTR(addr) ((uintptr_t)(uint32_t)(addr) + g_xbox_mem_offset)
```

### The Memory Offset

When the mapping lands at the original Xbox address (0x00000000), `g_xbox_mem_offset` is 0 and the MEM macros are identity casts. When Windows cannot map at the preferred address (common on Windows 11 where low addresses are reserved), the offset adjusts all accesses:

```c
// Set once during init, then read-only
ptrdiff_t g_xbox_mem_offset = (uintptr_t)actual_base - XBOX_MAP_START;
```

The implementation tries multiple base addresses in order of preference:

```c
static const uintptr_t try_bases[] = {
    0x00010000,  // Original Xbox address (ideal)
    0x00800000,  // 8 MB - above typical PEB/TEB
    0x01000000,  // 16 MB
    0x02000000,  // 32 MB
    0x10000000,  // 256 MB
    0,           // Let OS choose (last resort)
};
```

## On macOS and Linux: one 4 GB arena

The base list above is useless off Windows: every address in it is below
4 GB, and an arm64 macOS process has a 4 GB `__PAGEZERO` there that cannot be
shrunk. So on POSIX `xbox_MemoryLayoutInit` first reserves the whole guest
span: 4 GB plus a 64 KB guard, aligned to 4 GB, as one inaccessible range
(`w32_reserve_arena`, `src/platform/posix_memory.c`). Guest 0 is its base,
which is `g_xbox_mem_offset`. RAM, the mirrors, the contiguous window, the
tiled aperture and the device apertures are then placed inside it with the
same `MapViewOfFileEx` and `VirtualAlloc` calls Windows uses. The arena keeps
Windows' rule that a fixed placement lands exactly there or fails, because the
code here depends on a failed placement failing. The shared objects are
`memfd` on Linux and `shm_open` with an immediate `shm_unlink` on Darwin.

Apple Silicon's pages are 16 KB and the Xbox's are 4 KB. The arena keeps
every 4 KB page's requested protection in a side table and gives each host
page the most restrictive protection among its four guest pages. An access the
guest page allows but the host page refuses (the 12 KB beside a 4 KB trap
such as the PCRTC page, the AC'97 page or `RECOMP_TRAP_NULL`'s page zero) is
completed by the fault route through an always-writable second view of the
same memory (`w32_backdoor`, `recomp_fault_passthrough`). The main TIB
moved from 0x1000 to 0x4000 for this reason: on a 16 KB host, page zero's trap
covers 0..0x3FFF. `tests/guest_memory`, `tests/memory_layout` (the real layout
at 64 and 128 MB) and `tests/guest_faults` cover all of it.

Mirrors stop at 0x80000000 on every host. That is where the console's kernel
space starts (the contiguous window, then the tiled aperture and the
devices), and none of it is a wrap of low RAM. 28 mirrors of a 64 MB map end
at 0x74000000, so a retail map is unaffected. A 128 MB map (BLiNX) gets 15.

### Faults off Windows

On Windows a trapped page is serviced by the vectored exception handler. On
POSIX it is a `SIGSEGV`/`SIGBUS` handler (`src/platform/fault_posix.c`) on a
per-thread alternate stack. Every thread gets one through
`recomp_fault_thread_init`, which also pre-touches the handler's
thread-locals, because on Darwin the first use of a `__thread` variable
allocates. Both handlers build a `recomp_fault` (`recomp_fault.h`) and hand
it to the same route, `xbox_fault_route` (`src/kernel/xbox_fault_route.c`),
which tries these in order:

1. A single-step: Windows watchpoints only.
2. A watched page (`RECOMP_WATCH_WRITE`).
3. A registered device range. `apu_fault_register` adds the PCRTC interrupt
   page, the APU registers and the AC'97 page.
4. POSIX only: the 16 KB pass-through above.

Anything the route declines reaches the title's crash report.

"Completing" an access means decoding the host instruction, doing what it
would have done against a device model or the backdoor, writing the result
registers back into the signal context and stepping past it. The decoder is
chosen by the host CPU, not the OS. `mmio_decode.h` handles x86-64 (Windows
and Linux x86-64). `mmio_decode_a64.h` handles arm64, and is complete, not
device-shaped: on a 16 KB host the collateral 12 KB is ordinary guest memory
reached by whatever clang emitted. That covers every LDR/STR addressing mode,
sign-extending loads, FP/SIMD up to Q, all four LDP/STP modes, LSE atomics,
CAS, LDXR/STXR and DC ZVA. Guest-memory atomics are done as real atomics on
the backdoor, and an emulated LDXR/STXR pair becomes a compare-and-swap, so
they stay atomic against other threads. A dry run checks every element of an
access against the 4 KB side table before anything is done, so a pair that
straddles into a trapped page is refused whole rather than half-done.
`tests/mmio_decode_a64` compares the emulator against the hardware on about
100 assembled forms and on clang-compiled MEM-style code, memcpy, memmove and
memset.

### Measured (Gate A, 7 Oct 2026)

On an Apple M4 (macOS 27, 16 KB pages) at exp `47b78dd`, `RECOMP_HLE_D3D8=off`,
60 s:

- **TimeSplitters 2** reaches its first `Swap` straight after
  `Direct3D_CreateDevice` and runs 3569 Swaps in 60 s, about 59.5 fps. The
  Windows baseline is 58-59. There were no faults and no unresolved indirect
  calls, and 26,835 kernel calls. The PCRTC acknowledge, the APU start and
  the DSP doorbell all go through the device ranges.
- **BLiNX** maps 128 MB with 15/15 mirrors and takes no faults. It reaches its
  first `Swap` only with `RECOMP_GUEST_LOCK=0` (2072 Swaps in 40 s). With the
  guest lock on, the default on arm64, it starves before the first frame.
  That is a lock-fairness problem, not a memory one (fixed since: see "Guest
  threads" below).

These counts are `[TRACE swap]` lines from `RECOMP_HLE_D3D8_TRACE_SWAPS=0-1`;
`[FPS]` prints on POSIX too now.

## Guest threads: one at a time, by strict priority

With the guest lock on (`RECOMP_GUEST_LOCK`, the default on arm64), one guest
thread runs lifted code at a time, as on the console's one CPU. Which one is
the console's rule, and titles depend on it.

**Preemption happens only at a loop back edge or a blocking call, never at the
return of a kernel call that cannot block.** Put that first because it is the
rule that bit twice. The rest:

- The lock belongs to the highest-priority runnable guest thread. It keeps the
  lock until it blocks (a wait, a delay, I/O, a contended critical section,
  `xbox_GuestSleep`) or suspends itself.
- A waiter of higher priority takes over at the holder's next back edge
  (`RECOMP_BACKEDGE()`, at every loop header; the only preemption point lifted
  code has) or when the holder blocks. A non-blocking kernel call is an
  exception only when the call itself readied that waiter (a resume, the
  event it waited on). It then preempts at the call's return, as NT does. A
  waiter that was already waiting before the call does not.
- Equal priorities take turns at a back edge or a blocking call once the
  holder's quantum (`RECOMP_GUEST_QUANTUM_US`, 2000) is up.
- A lower priority runs only when everything above it blocks. That starves
  it, and the console starves it too.
- When the lock comes free, it goes to the highest-priority waiter, not to
  whichever wakes first (`guest_outranked`). A resumed thread counts as waiting
  from the moment the resume makes it runnable, not from when it wakes.
- Interrupts (the vblank ISR and DPCs, which run on host threads) outrank every
  guest thread and get in wherever a guest thread lets go, a non-blocking
  kernel call included.

The lock's mutex still has to go down around every kernel call: the bridge
cannot see whether a host call will block, and interrupts must get in. So a
call that `bridge_may_block` (in `kernel_bridge.c`) does not list keeps
ownership while the mutex is down (`xbox_GuestLockDropForKernel`): other guest
threads wait for it to come back, and the caller retakes the mutex before it
lets that ownership go. A thread suspending itself gives the ownership up
there and then. Two bounds keep a mistake here from becoming a hang, and both
say when they fire. A call that turns out to block hands over after
`RECOMP_GUEST_RESERVE_US` (50 ms), and the log names its ordinal for
`bridge_may_block`. A waiter starved for 500 ms (`GL_STARVE_QUANTA`) is let in
anyway, logged once per pair of threads with both tids and priorities. On the
console that would mean a priority here is wrong.

Priorities are the increments from `KeSetBasePriorityThread` (and
`KeSetPriorityThread` minus 8), kept by host thread id. `KeQueryBasePriorityThread`
answers from the same record. It used to answer 0 for every thread on every
host, including Windows, because it handed a guest object to `GetThreadPriority`.

The `[GUESTLOCK]` line every five seconds counts the handoffs at back edges and
blocking calls, how long interrupts waited, how often a non-blocking call kept
a thread out (and ran past its bound), how often a thread stood aside for a
higher priority, and how often the starvation valve let one in.
`RECOMP_THREAD_TRACE=1` prints every suspend, resume and priority change by
host tid, with the previous value. `RECOMP_GUEST_RESERVE_US=0` and
`RECOMP_GUEST_RANKED=0` switch the two newest rules off for an A/B run.

### Why: BLiNX's CRI ADX lock

CRI's ADX/Sofdec middleware (BLiNX; MvC2 has it too) locks like this,
`sub_000FA230`/`sub_000FA270` in BLiNX:

```c
lock:   if (count == 0) {                       /* [0x414AC4] */
            saved = GetThreadPriority(self);    /* one slot, [0xAAC358] */
            SetThreadPriority(self, 16);
            ResumeThread(spinner);              /* a priority-2 busy loop */
        }
        count++;
unlock: if (--count == 0) {
            SuspendThread(spinner);
            SetThreadPriority(self, saved);
        }
```

The count and the saved priority are plain globals. The lock is only correct on
a strict-priority uniprocessor. Nothing below 16 runs while the holder runs.
While it blocks, the spinner at 2 takes the CPU, so the file server at 1 and the
main thread at 0 stay out. Every one of these broke it here, each in its own
run, read off `RECOMP_THREAD_TRACE`:

1. Handing over at every kernel call. There are seven kernel calls between the
   test and the increment, so the file server got in between, both threads
   took the lock, and the spinner's suspend count ended at 2: it never ran
   again. A `ResumeThread` at count 0 is a no-op, there as on the console.
2. `KeQueryBasePriorityThread` answering 0. The server's first unlock
   restored it to 0, level with the main thread.
3. The mutex going to whichever waiter woke first. While the holder blocked,
   the server beat the spinner to it.
4. Clearing the reservation before retaking the mutex. A waiter took it in that
   gap, between `ResumeThread` and the increment.
5. Counting `SuspendThread` of another thread as blocking. A handoff
   between unlock's suspend and its priority restore let the main thread
   overwrite the saved slot.
6. A 40 ms starvation bound. A long turn at 16 reached it.
7. A resume and re-suspend before the thread woke. That left it counted as a
   waiter at 2, so everything lower stood aside for nothing, and BLiNX ran at
   1 fps.

A title that stalls only with the lock on, or whose threads end up at the wrong
priority, is the first thing to check against this list.

### Measured (7 Oct 2026, mac/kl-prio)

On a loaded Apple M4 (load average about 30 from other work):

- **TimeSplitters 2**, in the level: 59-60 fps with the rules and with them
  switched off (`RECOMP_GUEST_RESERVE_US=0 RECOMP_GUEST_RANKED=0`). The main
  thread used 31.1 s of CPU in 95 s with them and 33.1 s without. The
  starvation valve never fired.
- **BLiNX**, with the rules: 58-60 fps once past the logos. The trace shows
  the ADX lock intact for 90 s: the spinner's count only went 1, 0, 1; both
  threads restored their own priorities; no bound or valve fired. Without a
  trace, the valve fired once at boot, for the spinner behind the main thread
  at 16. Interrupts waited at most 65 ms while loading, then 1-10 ms.
- Both titles print one `a host thread waited 100 ms` warning at boot, with
  the rules or without. That is the main thread holding the lock across
  SDL's window and audio set-up, not scheduling.
- **Not fixed:** BLiNX still issues no read after opening `artoon001.sfd`, its
  second movie, now with the lock intact. An earlier run with the lock broken
  did get past it, to `op01.sfd`. So the remaining stall is somewhere else, and
  the Windows log is the comparison it needs.

## Section Initialization

After mapping the 64 MB region, the XBE file's sections are copied to their original addresses:

```c
// Copy XBE header (kernel thunk table, certificate, section info)
memcpy(XBOX_VA(0x00010000), xbe_data, header_size);

// Copy .text (code bytes -- needed for RW memory walker)
memcpy(XBOX_VA(0x00011000), xbe + 0x00001000, 2863616);

// Copy .rdata (constants, strings, vtables)
memcpy(XBOX_VA(0x0036B7C0), xbe + 0x0035C000, 289684);

// Copy initialized .data
memcpy(XBOX_VA(0x003B2360), xbe + 0x003A3000, 424960);

// BSS is already zeroed by the file mapping
```

Additional XDK library sections are also copied (XMV, DSOUND, WMADEC, XONLINE, XNET, D3D, XGRPH, XPP, DOLBY, XON_RD, .data1).

## Gotchas

### .rdata Is Not Write-Protected

You would expect .rdata (read-only data) to be protected:

```c
VirtualProtect(XBOX_VA(0x0036B7C0), 289684, PAGE_READONLY, &old_protect);
```

**Do not do this.** The .rdata end (0x003B2454) and .data start (0x003B2360) share the same 4KB page (0x003B2000-0x003B2FFF). VirtualProtect rounds to page boundaries, so making .rdata read-only also makes the first ~0xCA0 bytes of .data read-only. Game initialization code writes to globals in that overlap range and faults.

Additionally, the game writes to .rdata at runtime. String pointers in .rdata get overwritten during resource loading. This is technically a bug in the original game, but it works on Xbox because .rdata is not actually protected in the Xbox kernel's memory model.

### .rdata String Corruption

Because the game writes to .rdata at runtime, string data gets corrupted. Functions that read filenames from .rdata must read from the original XBE file data instead:

```c
// WRONG: reads from potentially-corrupted .rdata in mapped memory
const char *name = (const char *)XBOX_PTR(name_va);

// CORRECT: reads from pristine XBE file copy
extern const uint8_t *g_xbe_data;
size_t file_offset = (name_va - 0x36B7C0) + 0x35C000;
const char *name = (const char *)(g_xbe_data + file_offset);
```

### BSS Mirror Addresses Fail

Some BSS addresses (around 0x76000000) would need mirror views at addresses that Windows 11 reserves for system use. About 4 out of 33 mirror views fail to map. This is acceptable -- the game only accesses those addresses through the base view, and the RenderWare walker handles missing mirrors gracefully.

### Fake Thread Information Block

The recompiler drops the `fs:` segment prefix from memory accesses like `mov eax, fs:[0x28]`. These become `MEM32(0x28)`, reading from low memory. A fake TIB is populated at address 0x0:

```c
MEM32(0x00) = 0xFFFFFFFF;      // SEH: end of chain
MEM32(0x04) = XBOX_STACK_TOP;  // Stack base (high address)
MEM32(0x08) = XBOX_STACK_BASE; // Stack limit (low address)
MEM32(0x18) = 0x00000000;      // Self pointer
MEM32(0x20) = 0x00000000;      // KPCR Prcb pointer
MEM32(0x28) = FAKE_TLS_VA;     // TLS / RW engine context
```

The RenderWare engine reads `[fs:[0x28] + 0x28]` to find its per-thread data area. A fake structure chain is set up in BSS memory.

### Xbox Kernel PE Header

RenderWare's cache initialization code reads `MEM32(0x8001003C)` to parse the Xbox kernel's PE header and find the INIT section for CPU cache line sizing. A fake PE header with 0 sections is allocated at 0x80010000 so the function gracefully skips:

```c
void *kernel_page = VirtualAlloc((LPVOID)0x80010000, 4096,
                                  MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
memset(kernel_page, 0, 4096);
*(uint32_t *)((uint8_t *)kernel_page + 0x3C) = 0x80;  // e_lfanew
// NumberOfSections = 0, so the INIT section search finds nothing
```

## Dynamic Heap: Bump Allocator

The Xbox heap serves allocations from `MmAllocateContiguousMemory` and similar kernel functions. A simple bump allocator works because Xbox games rarely free memory:

```c
static uint32_t g_heap_next = XBOX_HEAP_BASE;  // 0x00880000

uint32_t xbox_HeapAlloc(uint32_t size, uint32_t alignment) {
    if (size < 4096) size = 4096;  // minimum to prevent overlapping zero-size allocs

    uint32_t result = (g_heap_next + alignment - 1) & ~(alignment - 1);

    if (result + size > XBOX_HEAP_BASE + XBOX_HEAP_SIZE)
        return 0;  // out of memory

    g_heap_next = result + size;
    memset((void *)((uintptr_t)result + g_memory_offset), 0, size);
    return result;  // returns Xbox VA, not native pointer
}

void xbox_HeapFree(uint32_t xbox_va) {
    (void)xbox_va;  // no-op
}
```

The minimum allocation size of 4096 bytes prevents a subtle bug: Xbox D3D8 code sometimes computes resource sizes from GPU capabilities that return 0 (no real NV2A hardware), causing zero-size allocations that all return the same address and overlap.

## NV2A GPU Registers

The Xbox GPU (NV2A) has memory-mapped registers at 0xFD000000+. Game code and XDK library code read/write these registers directly. On Windows, these addresses are not mapped by default.

A Vectored Exception Handler (VEH) intercepts access violations in this range and allocates pages on demand:

```c
LONG WINAPI nv2a_veh_handler(EXCEPTION_POINTERS *info) {
    if (info->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION)
        return EXCEPTION_CONTINUE_SEARCH;

    uintptr_t fault_addr = info->ExceptionRecord->ExceptionInformation[1];
    if (fault_addr >= 0xFD000000 && fault_addr < 0xFF000000) {
        // Allocate a page at the faulting address
        uintptr_t page = fault_addr & ~0xFFF;
        VirtualAlloc((LPVOID)page, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
```

The allocated pages are zeroed, so GPU register reads return 0 (safe defaults). NV2A functions that spin-wait on register values must be stubbed entirely -- allocating the page only prevents the crash; the spin-wait still loops forever on a zero value.

## Memory Layout Summary

```
0x00000000  ┌──────────────────────┐
            │ Fake TIB / KPCR      │  256 bytes
0x00010000  ├──────────────────────┤
            │ XBE Image Header     │  4 KB
0x00011000  ├──────────────────────┤
            │ .text (game code)    │  2.73 MB
0x002CC200  ├──────────────────────┤
            │ XDK library sections │  ~600 KB
0x0036B7C0  ├──────────────────────┤
            │ .rdata (constants)   │  280 KB
0x003B2360  ├──────────────────────┤
            │ .data + BSS          │  3.9 MB
0x00780000  ├──────────────────────┤
            │ Stack (8 MB)         │  g_esp starts at top
0x00F80000  ├──────────────────────┤
            │ Dynamic Heap         │  ~49 MB (bump allocator)
0x03FFFFFF  └──────────────────────┘  End of 64 MB region
            │ ... 28 mirror views  │  Each 64 MB, aliased to base
0x80010000  │ Fake kernel PE hdr   │  1 page
0xFD000000  │ NV2A GPU registers   │  On-demand VEH allocation
0xFE000000  │ Kernel thunks        │  Synthetic VAs for 147 imports
```
