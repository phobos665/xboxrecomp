# Sixth title: Black, XDK 5849, D3D8LTCG

Started 20 September 2026. Chosen because Criterion made it and Burnout is
already the reference implementation, so shared engine code was the bet.

It is the **first title in this fork on D3D8LTCG**, and it matches the
upstream proven target (Burnout 3) exactly on both XDK build and link mode.
That makes it the best available test of whether the LTCG signature work
generalises.

## What it is

| | |
| --- | --- |
| Title ID | 0x45410083 |
| XDK build | **5849** — same as Burnout 3 and Outrun 2 |
| Graphics | **D3D8LTCG** — the first LTCG title here |
| RenderWare | not detected |
| `.text` | 2076 KB |
| Image | 5599 KB |
| Kernel imports | 115 |
| Sections | 13, three demand-loaded |
| Entry point | 0x00025A3F |
| Concern flagged | a WMADEC section |

## It builds first time

Nothing in the pipeline needed changing, and this is the cleanest lift of the
six:

```
py -3 -m tools.xdk_symbols "games/Black/default.xbe"
py -3 scripts/recompile.py "games/Black/default.xbe" \
    --work-dir games/_pipeline/black/out --project titles/black
py -3 scripts/regen_title_main.py black "Black" 0x00025A3F "Black"
```

| | |
| --- | --- |
| Symbols recovered | 372, including **109 D3D8LTCG functions** |
| Functions | 13,478 found, 13,460 translated, **0 failed** |
| Lines of C | 1,282,719 in 14 files |
| Unresolved call stubs | **35** — the lowest of any title here |

The whole pipeline took 87 seconds. The LTCG signature database clearly does
work on this build, which was the open question.

**One gap in the tooling:** `scripts/recompile.py` writes the generated
sources and `regen_title_main.py` writes `main.c`, but nothing scaffolds the
project's `CMakeLists.txt`, `recomp_manual.c` or `.gitignore`. Those were
copied from another title by hand. Worth automating, since it is now the
sixth time.

## How far it got before seeding

This section and the two after it record the state before
`tools.seed_from_log` was run, and are kept because the reasoning in them is
what led to the seeding. The conclusion they reach is **wrong**; see
"Seeding changes everything" below.

A long way, and with no crash at all:

- the kernel thunk bridge, 115 of 115 resolved, 5 stubs
- the heap, the file system, `PsCreateSystemThreadEx` for a worker
- the AC97 codec, XAudio2 at 48 kHz, the APU
- the vblank clock at 60 Hz and the flip gate

Then it stops, without faulting. Thirty-five seconds in it has made
**24.5 million `KeWaitForSingleObject` calls** and drawn nothing. There is no
`Swap`, no `[FPS]`, and no `[HLE-D3D8]` line anywhere in the log, which means
**`Direct3D_CreateDevice` is never called**.

## Where it actually is

`RECOMP_SAMPLE=1000` names it in one run, with no trace build:

```
  [guest main   tid 15060 ]  5020 samples, 94.1% on-CPU, 9.4s CPU time
       97.00%  sub_0026D0C0  [black_recomp.exe]
      inclusive (on the stack), hottest first:
       97.27%  sub_000259CB ... sub_00170C80 ... sub_0026CF10
               ... sub_0026CDC0 ... sub_0026D0C0
```

Matching those against the recovered symbols gives the chain. Game code at
`0x00170C80` calls `CDevice_KickOff` (0x0026CF10), which calls
`D3D_MakeRequestedSpace` (0x0026CDC0), which calls the spin:

```c
    eax = MEM32(0x27BFF8);            /* the D3D8 device global */
    eax = MEM32(eax + 0x1C28);
    ecx = MEM32(eax + 0x100410);
    ecx = ecx | 0x10000;              /* ring the doorbell */
    MEM32(eax + 0x100410) = ecx;
loc_0026D0E0:
    test MEM32(eax + 0x100410), 0x10000
    jne  loc_0026D0E0                 /* spin until the GPU clears it */
```

That is the NV2A "wait until idle" handshake: set a bit in a register and
spin until the hardware clears it. Nothing here clears it, so it spins
forever.

**This is the push-buffer path, and `CLAUDE.md` already names it as the open
gap**: "A path for titles that fill push buffers through `BeginPush` is still
the next piece of work: those draws reach no replacement at all." Black is the
first title here that takes it.

## The other half: the device global is null

Worse than a missing replacement, the spin is operating on garbage.
`MEM32(0x27BFF8)` is **zero**, so `[eax + 0x1C28]` reads guest address
0x1C28, which is inside the main thread's own information block, and the
doorbell is written through whatever that happens to contain.

A watchpoint settles what writes that global:

```
RECOMP_WATCH_WRITE=0x0027BFF8 ...
[WATCH] watching 0x0027BFF8 (4 bytes), currently 0x00000000
```

**Nothing ever writes it in a whole run.** Statically there are three writers,
and the one that matters is `MEM32(0x27BFF8) = edi` inside
`sub_0026C620_hle_original` — which is to say inside the original body of
**`Direct3D_CreateDevice`**. So the global is null because the device was
never created, and the title is running its render path anyway.

The same shape shows in the worker thread. It waits forever on an object at
guest 0x00001DBC, whose dispatcher header is entirely zero, and the call site
computes that address as `*(0x27BFF8) + 0x1DBC` — the same null global plus an
offset.

## Seeding changes everything

The 190 unresolved indirect-call targets turned out to be the whole story,
not a side issue. `tools.seed_from_log` recovers them from a run's log:

```
py -3 -m tools.seed_from_log games/_pipeline/black/runs/icall.err \
    "games/Black/default.xbe" \
    --functions games/_pipeline/black/out/disasm/functions.json \
    --analysis-json "games/Black/default_analysis.json"
```

192 seeds, re-lift, rebuild. The function count barely moves -- 13,479 against
13,478 -- because the addresses were already known to the disassembler. What
changes is that they now get **dispatch entries**, so an indirect call to one
resolves instead of being refused.

That is the difference between no device and a rendering title:

```
D3D8: Device created (640x480)
[HLE-D3D8] CreateDevice set target 0x0027B574, depth 0x0027B5BC
[HLE-D3D8] GPU time fence mirrored: device +0x2C -> *(device +0x30),
           offsets read from D3D_BlockOnTime
[HLE-D3D8] shadow device 640x480, from the title's own CreateDevice parameters
[HLE-D3D8] shadow vertex shader 0x00F83961: host program
[HLE-D3D8] shadow declaration 0x00F83961 flags 0x10: v0=s0+0:42 ...
```

So `Direct3D_CreateDevice` is reached, the 5849 fence offsets (+0x2C and
+0x30) are read correctly out of `D3D_BlockOnTime`'s prologue, the shadow
renderer comes up, and the title's own vertex programs are being translated to
host shaders. Three `Swap` calls happen.

**The push-buffer spin and the null device global were both consequences of
the missing dispatch entries**, not separate problems. The earlier reading of
this page -- that Black needs the push-buffer path built before it can run --
was wrong, and is retracted. It may still need it later; it does not need it
to get this far.

The lesson is procedural and applies to every title: **run once, seed from the
log, re-lift, and only then start diagnosing.** A refused indirect call is not
a small thing that can be cleaned up afterwards. It silently removes whatever
that function did.

## Where it stops now

It crashes, rather than hanging:

```
[CRASH] Access violation at RIP=..., fault addr=0x10000FFF0 (write)
  Xbox regs: eax=0xFFF08055 ecx=0xFFFFFD80 edx=0xFFFFFFF0 esp=0x00F7FA74
  Xbox regs: ebx=0x00000001 esi=0x01004B2F edi=0xFFFFFFF0
  Xbox VA of fault: 0xFFFFFFF0
```

0xFFFFFFF0 is -16, which is a null pointer with a negative offset applied
rather than a wild value. 201 calls are still unresolved in this run, so the
first thing to try is another seeding round before reading anything into the
fault.

## What to look at next

1. **Seed again from the latest run.** 201 calls are still unresolved. The
   first round took the title from a dead spin to a rendering device, so this
   is the cheapest thing to do and should be exhausted before anything else.
2. **Then the write to 0xFFFFFFF0**, if it survives that.
3. The three demand-loaded sections have not been exercised.

## Fixed along the way

The `[WAIT]` diagnostic was reporting the wrong caller. It read the top of the
guest stack inside the bridge body, where the return address has already been
popped and the first argument sits instead, so every wait appeared to be
called from the object it was waiting on. The dispatcher now captures the
guest return address into `g_kernel_caller` before the bridge runs, which is
what turned "waits on 0x1DBC, caller 0x1DBC" into a real call site and made
the rest of this findable.
