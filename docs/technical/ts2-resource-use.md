# TimeSplitters 2: what the recompiled build costs, against xemu

19 September 2026. Both run the same game, on the same machine, in the same place
in it — the Siberia level, in the tunnel at the start.

## The machine

| | |
| --- | --- |
| OS | Windows 10 Pro 19045 |
| CPU | Intel Core i7-12700H, 14 cores / 20 logical, 2.3 GHz base |
| RAM | 32 GB |
| GPU | NVIDIA GeForce RTX 3050 Ti Laptop (4 GB) **and** Intel Iris Xe (shared) |

The two GPUs matter: **the recompiled build ran on the Intel Iris Xe and xemu
rendered on the RTX** (`xemu.log`: `GL_RENDERER: NVIDIA GeForce RTX 3050 Ti`),
presenting through the Intel. The GPU percentages below are therefore percentages
of different hardware and do not compare. Everything else does.

## Method

`scripts/measure_resources.ps1 -ProcessId <pid> -DelaySeconds N -Seconds 50`,
one sample a second over the window, mean and max of each. Counters, all
per-process:

- `\Process(<name>)\% Processor Time` — 100 = one logical core saturated. "% of
  total" below is this divided by 20, the logical core count.
- `\Process(<name>)\Working Set` and `\Working Set - Private` — resident, with
  and without shareable pages.
- `\Process(<name>)\Private Bytes` — committed and not shareable.
- `\GPU Engine(pid_<pid>_*)\Utilization Percentage` — summed over the process's
  engines, and the largest single engine separately.
- `\GPU Process Memory(pid_<pid>_*)\Local Usage` and `\Dedicated Usage`.

The first sample of each rate counter is discarded: it has no previous raw value
to difference against.

**Recompiled build.** `titles\timesplitters2\build\Release\timesplitters2_recomp.exe`,
built 19 Sep 17:20 from `perf/ts2-items-1-7` (96c1040 plus a scissor fix), run
with the working directory set to `build\Release` and
`RECOMP_VBLANK=1 RECOMP_AC97_READY=1 RECOMP_HLE_D3D8=shadow RECOMP_TRACE_BUDGET=0 RECOMP_FPS=5`,
once with the default adaptive frame cap and once with `RECOMP_FPS_CAP=0`.
Gameplay was reached with `RECOMP_INPUT_SEQ`, save profiles cleared first.
Window **t = 195–245 s**.

**xemu.** 0.8.136, started and played to the level by hand — not scripted.
Windows counters only; xemu was left exactly as configured. Window
**17:48:12–17:49:17** for everything but `Private Bytes`, which was sampled in a
second window, **17:51:06–17:51:50**, on the same process in the same place.

### Reaching the level without a human, and how to know you did

The path in `docs/technical/second-title-bringup.md` stops one press short on
this build: its twelve presses end on the level-select screen, and a run that
sits there reports a perfectly steady 57 fps that looks like gameplay. Four more
`a` presses reach the level:

```text
RECOMP_INPUT_SEQ=12000:start,16000:start,20000:start,24000:start,28000:a,34000:a,40000:a,46000:a,52000:a,58000:a,64000:a,70000:a,76000:a,82000:a,88000:a,94000:a
```

With those, the level loads at ~t = 115 s, the opening cutscene runs to ~t = 180 s
and the level is drawing from then on. Confirm it from the stderr rather than
from the frame rate — `[HLE-D3D8] shadow:` draw totals differenced over swaps
give ~190 draws a frame in the front end and 370–470 in the level, which is the
check `docs/technical/ts2-performance-plan.md` prescribes. Both windows here were
verified that way and by a screenshot taken mid-window.

The save profile has to be gone before **every** run, not just the first: a run
writes one at about t = 40 s, so the second run down a script takes a different
menu path than the first.

## The numbers

Mean (max) over 50 s of gameplay for the recompiled build, 60 s for xemu.

| | recompiled, capped (default) | recompiled, `FPS_CAP=0` | xemu 0.8.136 |
| --- | --- | --- | --- |
| Frame rate | **57.4 fps** (56.4–58.3) | **184 fps** (173–191) | not readable — see below |
| CPU, % of one core | 145 (176) | 251 (274) | 244 (284) |
| CPU, % of all 20 cores | 7.2 (8.8) | 12.5 (13.7) | 12.2 (14.2) |
| Working set | 241 MB (241) | 236 MB (236) | 723 MB (725) |
| Working set, private | 149 MB (150) | 145 MB (145) | 639 MB (641) |
| Private bytes | not measured | not measured | 2,400 MB (2,401) |
| GPU, sum over engines | 8.3 % (10.8) | 23.1 % (26.1) | 51.1 % (58.5) |
| GPU, largest single engine | 8.3 % (10.8) | 23.1 % (26.1) | 33.7 % (40.3) |
| GPU memory, local | 76 MB (81) | 65 MB (76) | 242 MB (249) |
| GPU memory, dedicated | 0 MB | 0 MB | 213 MB |
| GPU adapter | Intel Iris Xe, one 3D engine | same | RTX 3050 Ti (3D + copy) **and** Intel (3D) |

Read across the memory rows and the recompiled build uses **about a third of
xemu's resident memory** — 241 MB against 723 MB — and a fifth of its private
resident memory. Its committed private bytes were not captured (the counter was
added to the script after those runs and xemu was occupying the machine
afterwards); xemu's 2.4 GB is mostly QEMU's reservations rather than pages in
use, which is why its working set is a quarter of it.

CPU is the one place the two are close when the recompiled build is let run
free: 251 % of a core against xemu's 244 %, i.e. both keep about two and a half
cores busy. The difference is what they get for it — 184 fps against xemu's,
which could not be measured but cannot exceed a small multiple of the console's
60 Hz. Capped to the console cadence the recompiled build costs 145 % of a core,
a little over half of what xemu spends.

## Frame rate

The recompiled build prints its own: `RECOMP_FPS=5` gives an `[FPS]` line every
five seconds. In the window, capped: 57.6, 58.0, 57.4, 57.1, 57.0, 56.4, 58.3,
56.5, 58.0 — mean 57.4, which is the adaptive gate holding one present per
vblank on a 58.8 Hz clock. Uncapped: 189.3, 191.4, 187.0, 188.5, 190.4, 173.2,
182.7, 176.8, 183.2, 176.8 — mean 184. That is roughly double the 80–89 fps the
items 1–7 baseline measured on 19 Sep, because those items landed.

**xemu's frame rate could not be read.** Its title bar is `xemu | v0.8.136` with
no counter in it, `xemu.log` records none, and its on-screen statistics overlay
was off — turning it on means changing the user's settings, which this
measurement did not do. So the fps column has a hole in it on xemu's side, and
the honest comparison here is the resource columns.

## Caveats

- **The GPU columns are not comparable.** Different adapters, different APIs
  (D3D on the Intel iGPU against OpenGL on the RTX), and different output sizes:
  xemu was played in a window about 1210×880, the recompiled build's shadow
  renderer is about 800×600. Its 8 % of an Iris Xe and xemu's 34 % of an RTX
  3050 Ti say nothing about each other.
- **The recompiled build's private bytes are missing**, for the reason above.
  One more 4½-minute run fills it.
- **xemu was driven by hand**, so its window is 60 s of someone playing, while
  the recompiled build's is 50 s of a scripted path standing in the same tunnel.
  Player input costs more than standing still.
- **The machine is shared.** The runs reported here were made with no compiler
  running; earlier attempts that overlapped a build were discarded, and so were
  two runs that turned out to be sitting in the front end rather than the level.
  Eight idle `MSBuild.exe` node-reuse processes were resident throughout and used
  0 % CPU.
- `Working Set - Private` is resident private memory, not `Private Bytes`; both
  are given for xemu because they differ by a factor of nearly four there.
- The recompiled build's memory model maps guest address space through a file
  mapping, which is shareable and so does not appear in the private rows.
