# Third title: Marvel vs Capcom 2, XDK 5028

Started 19 September 2026. A sprite-based 2D fighter, picked to find out what
a 2D title costs compared with the two 3D ones — and, incidentally, because it
is a third XDK version, which is what separates "version-specific" from
"wrongly treated as universal".

## What it is

| | |
| --- | --- |
| Title ID | 0x43430007 |
| XDK build | **5028** (TimeSplitters 2 is 4721, Burnout 2 is 5344) |
| Graphics | D3D8, not LTCG |
| Engine | not RenderWare |
| `.text` | 2,504 KB |
| Sections | 10, one demand-loaded |
| Kernel imports | 124 |
| Entry point | 0x0020EE2F |
| Disc data | 241 MB, mostly `.AFS` archives |

## Getting to a build

Nothing in the pipeline needed changing, which is worth recording on its own:
a third XDK, a different publisher's engine, and all four stages ran as
written.

```
py -3 -m tools.xdk_symbols "games/Marvel Vs Capcom 2/default.xbe"
py -3 scripts/recompile.py "games/Marvel Vs Capcom 2/default.xbe" \
    --work-dir games/_pipeline/mvc2/out --project titles/mvc2
py -3 scripts/regen_title_main.py mvc2 "Marvel vs Capcom 2" 0x0020EE2F "Marvel Vs Capcom 2"
cmake -S titles/mvc2 -B titles/mvc2/build -G "Visual Studio 16 2019" -A x64
cmake --build titles/mvc2/build --config Release
```

- 114 seconds for all four stages. **21,807 functions, 1,774,511 lines of C,
  22 files, none failed**, 68 call targets stubbed as unresolved.
- **46 of 53 XDK functions replaced by name**, so the D3D8 replacement engages.
  Run `tools.xdk_symbols` *before* the pipeline: without
  `default_xdk_symbols.json` beside the XBE the lift silently replaces nothing,
  and the title then renders through the emulated NV2A, which draws nothing.
  `scripts/recompile.py` does not run that stage.

## Where it stops

It boots, opens the renderer's window, **presents exactly one frame**, and then
makes no further progress. No crash, no fault, no hang in the usual sense —
the process stays alive and the vblank keeps ticking at about 59 Hz.

The GPU time fence is mirrored correctly, so this is *not* the
`D3D_BlockOnTime` case from `CLAUDE.md`'s debug table despite looking like it
from outside. The 5028 prologue matched the one the HLE already knows:

```
[HLE-D3D8] GPU time fence mirrored: device +0x2C -> *(device +0x30),
           offsets read from D3D_BlockOnTime
```

What it is doing instead, from a 4,000-call kernel log, is a worker thread
(stack around 0x0119CF38, not the main thread) going round this loop forever:

```
ObReferenceObjectByHandle   (246)
KeSetBasePriorityThread     (143)   or KeQueryBasePriorityThread (124)
ObfDereferenceObject        (250)
NtSuspendThread             (231)   and NtResumeThread (224)
```

Thousands of iterations: 3,586 resumes and 3,582 suspends in a 45-second run,
against 2,800 `KeWaitForSingleObject`. The return addresses all fall in a
cluster of small functions around 0x0020E5CD, which look like the XAPI's own
`SetThreadPriority` / `SuspendThread` / `ResumeThread` wrappers rather than
game code — so the interesting question is which caller is driving them.

Meanwhile the main thread (stack 0x007BFFCC) is alive and calling
`KeInsertQueueDpc` and `KeSetEvent`, so nothing is deadlocked in the Win32
sense. This is a title running its own scheduler and never getting the answer
it wants.

## Next

1. **Find the caller.** The wrappers are library code; walk up from
   0x0020E5CD's callers to the game function driving the loop, and work out
   what condition it is waiting on.
2. **Suspect the threading model before the graphics.** The Xbox is
   uniprocessor and this title schedules with real suspend and resume, which
   is exactly the case `CLAUDE.md` planned change 4 is about. A scheduler
   written for one core, run on real Win32 threads across many, can livelock
   without anything being wrong per call.
3. Only then the rendering, which is the reason for picking this title: a 2D
   sprite game's draw pattern, and what it costs, against the two 3D ones.

Nothing here needed a per-title override yet, and it would be good to keep it
that way.
