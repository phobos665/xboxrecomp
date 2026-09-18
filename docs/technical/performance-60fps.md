# Getting Burnout 2 to 60 fps

Burnout 2 is a 60 fps title, so 60 is the bar and not a stretch. This document
is the starting point for that work: what is already measured, what the
suspects are, how to tell them apart, and which mistakes make a number mean
nothing.

Everything below is either a counted fact or a named hypothesis, and each one
says which it is. The first round of measurement is done — see **What the
measurements settled** — and it removed the two biggest suspects.

---

## Where it stands

**23.4 swaps per second** — 5,618 swaps over a 240 s run (18 September 2026,
commit `dab9f9a`, Release build). That is 39% of the target.

That run had instrumentation on, so it is an upper bound on the cost, not a
measurement of the shipped configuration:

| Switch | What it was doing |
|---|---|
| `RECOMP_PB_SCAN=1`, `RECOMP_PB_EXEC=1` | parsing *and executing* the title's push buffer |
| `RECOMP_HLE_D3D8=shadow` | drawing the whole frame again through D3D11 |
| `RECOMP_HLE_D3D8_DUMP` | reading back the host back buffer and writing BMPs |
| kernel log budget 200 | per-call logging |

**60 Hz is the target, not a ceiling.** The vblank the title paces on is a
fixed 60 Hz tick (`src/kernel/kernel_bridge.c`, `kernel_vblank_tick`, 16 ms),
delivered from the pump thread. At 23 swaps/s the title is missing that clock,
so the loss is ours: CPU, the HLE boundary, or the host renderer.

### Draw volume, counted

- **690–910 draws in a race frame**, 2–6 in a loading or menu frame.
- Over 240 s: 38,289 `DrawPrimitiveUP` + 278 indexed UP + 360,186 indexed
  buffer draws. The indexed-buffer path dominates.
- 0 draws skipped, 0 failed, 0 off the swapping thread.

---

## What the measurements settled

*18 September 2026. Every row uses the same instrument: `RECOMP_FPS=<seconds>`
counts the title's own `Swap` before anything else in the replacement runs, so
it means the same thing whatever else is switched on. 200 s runs, Release.*

### The renderer is not the bottleneck

| Configuration | fps (mean of 10 s windows) |
|---|---|
| shadow on, push buffer on (as shipped) | 15.1 |
| shadow on, push buffer **off** | 15.5 |
| shadow **off**, push buffer on | 15.5 |
| shadow **off**, push buffer **off** | 15.0 |

Turning off *all* host drawing and *all* push-buffer work changes nothing.
**Suspects 1, 2 and 3 below are ruled out as the current limiter** — the
per-draw state scan, the per-draw hashing and allocation, and the executor
running alongside shadow mode are all real inefficiencies, and none of them is
what is costing the frame rate today. Fixing them first would have been wasted
work, which is what this table bought.

### The frame clock was capping everything at 40 Hz

`kernel_vblank_tick` scheduled the next frame at `GetTickCount64() + 16`.
That counter advances in system timer ticks — 15.6 ms by default — so the
deadline could not be met before two of them. **Measured: 40.0 Hz delivered,
never 60.** A title that waits whole frames then quantises to 40, 20, 13.3 fps,
which is exactly the spread this project was seeing (13.1, 13.3, 12.6 …).

Fixed: the clock now runs off `QueryPerformanceCounter`, the pump thread asks
for a 1 ms system timer and sleeps to the deadline rather than a fixed
millisecond. **Measured: 60.0 Hz delivered.**

**It does not raise the frame rate today, and it slightly lowers it** — 14.2
mean against 15.1 at 40 Hz, because each tick runs the title's own ISR and DPC
chain, and there are now half as many again of them. The ceiling is gone; the
floor is elsewhere. `RECOMP_VBLANK_HZ` sets the rate for A/B without a
rebuild.

### So the cost is the guest side

What is left after the renderer, the push buffer and the clock: the lifted CPU
code, the kernel bridge, and memory. That is suspects 4 and 5, and neither has
been profiled yet.

**The next measurement**, and the one this document should be updated with:
`RECOMP_TRACE_PROFILE` over a race, read with `scripts/stall_report.py`, to
find where guest time goes. Its blind spot is in the Method section below.

---

## The first job: measure the shipped configuration

Before optimising anything, get a clean number.

```bash
# Nothing on but what the title needs to run and draw.
RECOMP_VBLANK=1 RECOMP_AC97_READY=1 RECOMP_APU_DSP_ACK=0x80BB0810,0x80C18810 \
RECOMP_USB=1 RECOMP_TRACE_BUDGET=0 RECOMP_HLE_D3D8=shadow \
RECOMP_FAKE_INPUT=start,down,down,a,left,a RECOMP_FAKE_INPUT_MS=4000 \
your_game_recomp.exe
```

Then read the swap count out of the shadow report line at exit and divide by
the run length. `scripts/run_and_report.py` records the switches in a `.meta`
beside each run, and refuses comparisons across different switch sets — use
it rather than hand-rolled runs, because a comparison across `RECOMP_VBLANK`
is a comparison of two different programs.

**Do not measure with `RECOMP_HLE_D3D8_DUMP` or `RECOMP_D3D8_CAPTURE` set.**
A dump reads back the back buffer; a capture writes every draw's vertices to
disk. `..._MINDRAWS` makes the capture case far worse, because it records and
deletes every frame until one is big enough.

---

## Separating the three costs

Frame time goes to three places, and they need different fixes:

1. **Guest CPU** — the lifted x86: physics, AI, the title's own D3D8.
2. **The HLE boundary** — `src/hle`, converting and forwarding per draw.
3. **The host renderer** — `src/d3d` and the GPU.

The instrument that separates them already exists. **A frame capture replays
with no game running**, so it measures 2+3 without 1:

```bash
d3d8_replay caps/race.d3dcap --loops 200 --quiet   # time it
```

That gives host-side cost per frame directly. Subtract it from the live frame
time and the remainder is the guest CPU plus whatever the capture does not
carry. Two runs of the same capture also differ only where the code changed,
which no pair of live runs can claim.

A second useful split: `RECOMP_HLE_D3D8=` unset runs the title with no shadow
drawing at all. The difference between that and a shadow run is the whole cost
of 2+3 in situ.

---

## Suspects

Ordered by expected size. Each is a hypothesis with a stated reason, not a
finding.

### 1. The push-buffer executor runs alongside shadow mode — RULED OUT

`RECOMP_PB_EXEC=1` executes the title's push buffer in the LLE path *while*
shadow mode draws the same frame through D3D11. If shadow mode now renders
everything the title asks for, the executor may be duplicated work.

**Experiment:** a run with `RECOMP_PB_EXEC=0` (and `RECOMP_PB_SCAN=0`).
**Watch for:** the title may depend on the executor for something shadow mode
does not forward — `BeginPush` traffic reaches no replacement, per
`docs/technical/shadow-mode.md`. Compare frames, not just the swap rate.

### 2. Per-draw state scanning — RULED OUT as the current limiter

`src/hle/hle_d3d8_state.c` reads the title's own state arrays on **every
draw** — up to 167 render states plus 4 × 32 texture stage states — and
compares each against `g_prev_rs` to find what changed. At ~900 draws a frame
that is ~270,000 guest memory reads per frame before anything is forwarded.

**Experiment:** count how many of those comparisons actually differ per frame.
If it is a handful, the scan is the cost and a dirty-bit or a change counter
on the guest side would replace it.

### 3. Work repeated per draw in the host layer — RULED OUT as the current limiter

- `d3d8_vsh_prepare_draw` hashes the **whole vertex program microcode**
  (FNV-1a) on every draw to look up the shader cache.
- `hle_d3d8_texture.c` checksums texture texels on binds to decide whether to
  re-upload.
- `shadow_expand_vertices` (`src/hle/hle_d3d8.c:1415`) **mallocs and copies
  every vertex** of a NORMPACKED3 draw, per draw; the triangle-list
  conversions near it allocate per draw too.

All three are per-draw allocations or O(size) passes where a cached handle or
a reused scratch buffer would do.

### 4. The register model — still open, now the leading suspect

`CLAUDE.md` plans this already: lifted code keeps guest registers in globals
(`g_eax` and friends) plus a simulated stack in a guest memory array. That
defeats aliasing analysis and register allocation across the whole lifted
body, which is the standing reason naive lifted output loses to a JIT. It is
also the most invasive change here, and it gets baked into generated code —
so measure before deciding it is the problem.

### 5. Logging that survives into a "clean" run — still open

`RECOMP_TRACE_BUDGET=0` silences `[TRACE]`, but the kernel log, the `[PATH]`
and `[READ]` file lines and the D3D8 first-call lines are separate. An earlier
session measured the profile screen polling a save directory every frame and
dropping the run to ~45 swaps/s on logging alone.

---

## Method

1. Measure the clean configuration. Write the number down with its switches.
2. Split it with replay: host cost per frame vs everything else.
3. Profile whichever side dominates. For the guest side,
   `RECOMP_TRACE_PROFILE=<interval>` plus `scripts/stall_report.py`; note the
   profile records **entries only** and is written at report intervals, so a
   function entered after the last report reads as "never ran".
4. Change one thing. Re-measure the same way. Keep the negative results — "this
   is provably right now and changed nothing" saves the next person deriving
   it again.

## Traps

- **A number from an instrumented run is not the shipped number.** Most of the
  switches in the run scripts exist for debugging and cost real time.
- **Release, not Debug.** The build target is `your_game_recomp`, Release.
- **Runs never land on the same moment twice**, so two live runs are not
  comparable frame for frame. Use a capture when the question is about one
  frame.
- **The shadow window pumps on its own thread** and present interval is
  already 0, so neither vsync nor window pumping is the limiter.
- **Check the swap counter, not wall-clock feel.** `RECOMP_FPS=10` prints the
  title's own present rate every ten seconds, and the vblank rate beside it.
  The shadow report line at exit carries swaps, clears and per-path draw
  counts, but it needs shadow mode, so it cannot compare configurations.
- **Frame rate swings with what is on screen** — a loading screen, a fade and
  a race are different workloads, and a 10 s window catches whichever it
  lands on. Compare means over a whole run, and treat a single window as
  noise.

## Where the ceiling actually is

Worth holding onto while reading any profile: per `CLAUDE.md`, the guest is
x86-32 and the host is x86-64, so the translation is same-ISA and a 733 MHz
Coppermine leaves enormous headroom. **If this title cannot hit 60, the cost
is far more likely to be in what the project does per draw than in the lifted
arithmetic itself.**
