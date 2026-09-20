# Marvel vs Capcom 2: renders, with a racy start-up

20 September 2026. The title **renders and plays sound**. What stops it being
usable is that roughly two runs in three never get going at all.

---

## Where it is

After the LOCK-prefix lifter fix (see `third-title-jsrf.md`) and 30 seeded
indirect-call targets, a good run reports:

```
shadow: 745 swaps, 2975 clears | draws: 1302 UP + 4317 indexed buffer drawn;
        skipped 0 program without layout, 0 declaration shader,
        0 unknown shader, 0 stride, 0 primitive, 0 failed
swap timing: rest of frame 24.84 ms
```

Nothing skipped, nothing failed, about **40 fps**. Confirmed by ear and eye
from a play session, not only from counters.

Before the fix it had 33 branches after `lock xadd` resolved against the
`_flags` placeholder, and 26 unresolved indirect-call targets.

---

## The problem: start-up succeeds about one run in three

Three consecutive 100-second runs, same binary, no switches:

| run | swaps | frame time |
| --- | --- | --- |
| 1 | 623 | 24.84 ms |
| 2 | 0 | never rendered |
| 3 | 0 | never rendered |

It is a race, not a wrong value: the same build does both.

**The two outcomes have different kernel profiles.** Ordinal counts at the end
of a run tell them apart at a glance:

| | a run that renders | a run that stalls |
| --- | --- | --- |
| ObReferenceObjectByHandle (246) | 26622 | — |
| ObfDereferenceObject (250) | 26622 | — |
| KeSetBasePriorityThread (143) | 17747 | — |
| NtResumeThread (224) | 14523 | 5828 |
| KeWaitForSingleObject (159) | — | 5827 |
| KeSetEvent (145) / KeInsertQueueDpc (119) | — | 5879 / 5877 |

A stalled run's top two are the vblank ISR pair, which means the main thread
is doing nothing and only the interrupt is still ticking. Below that,
`NtResumeThread` and `KeWaitForSingleObject` run in lockstep — 5828 against
5827 — so the main thread is resuming a worker and waiting for it to signal,
about 5800 times, and not getting what it waits for.

That handshake is the thing to chase. `bridge_NtResumeThread` and
`bridge_KeWaitForSingleObject` both look right on inspection, so the fault is
more likely in when the worker actually starts relative to the resume than in
either call.

---

## Two hypotheses that were measured and are wrong

Recorded because both were plausible and both cost a round of measurement.

**"The guest spin starves the loader."** The main thread sits in

```
enter CS; if (get_work() == -1) { leave CS; }   repeat
```

releasing that critical section 460 million times in 40 seconds, with the
sampler showing 28.9% of its time in `RtlAcquireSRWLockShared`. A switch was
added to yield on every *n*th release. Loader throughput did not move at all
across n = 0, 64 and 1024 — identical offsets and identical bytes read. The
switch was removed again.

**"Yielding fixes the frame rate."** A later single run with the yield on
looked like it had taken the frame from 758 ms to 25 ms. Repeating it in pairs
killed it:

```
CS_YIELD=0    swaps=0     no timing     <- a stalled run, not a slow one
CS_YIELD=256  swaps=643   24.98 ms
CS_YIELD=0    swaps=627   23.35 ms      <- the same, without yielding
CS_YIELD=256  swaps=601   24.89 ms
```

The apparent gain was one unpaired sample against a stalled run. **Because
start-up is a coin flip here, no single run of this title measures anything.**
Run it at least three times before believing a number — that applies to the
next person's experiment as much as it did to these two.

The 758 ms frame time from the first long run is the same artefact: an average
over a run that spent most of its length stalled before it began rendering.

---

## Counting swaps

`grep "shadow: Swap"` counts the one-off notice about which guest thread
swaps, not swaps. The number is in the periodic summary line:

```bash
grep -oE "shadow: [0-9]+ swaps" run.err | grep -oE "[0-9]+" | sort -n | tail -1
```

Reading the wrong one made this title look like it rendered a single frame
when it was rendering hundreds.
