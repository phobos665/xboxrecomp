# TimeSplitters 2: what is still wrong

Shelved 19 September 2026, with the title playable. This is the list to pick
up from, in the order I would pick it up. Everything here was reproduced, and
the evidence for each is named so nobody has to find it twice.

Where things stand: the title boots, saves a profile, loads Siberia, renders
it lit, plays its music and effects, takes keyboard and pad input, and holds
60 fps under the default frame cap. `docs/technical/second-title-bringup.md`
is how it got there.

---

## 1. The picture is about a third of xemu's brightness

**The most visible thing wrong, and the least understood.** Same surfaces,
measured from a screenshot of each:

| surface | xemu | this build |
| --- | --- | --- |
| snow | 183, 193, 215 | 67, 65, 58 |
| wooden plank | 152, 129, 82 | 67, 59, 36 |
| building wall | 82, 95, 123 | 60, 72, 70 |

Not only darker: xemu's snow is blue-white, as a snowy night should be, and
ours is flat grey-brown, so the colour cast is gone too.

**What is known.** Three translucent full-screen quads are drawn over the
finished scene every frame. In a captured gameplay frame they take it from
(54, 58, 68) to (20, 22, 26) — a factor of 0.38. Replaying that capture and
stopping one draw before them gives a bright, correct frame with the blue cast
xemu has. Reproduce with:

```
d3d8_replay games/_pipeline/timesplitters2/cap_sh/sh_05900.d3dcap --out before --draws 343
d3d8_replay games/_pipeline/timesplitters2/cap_sh/sh_05900.d3dcap --out after
```

**What was ruled out.** The textures those quads sample are all zeros in the
capture, so the obvious theory was that the title reads its own screen back
and got black. That theory is dead: the title makes a texture whose texels
*are* the frame buffer, that is now recognised and filled with the host's own
frame (`src/d3d/d3d8_screencopy.c`, `framebuffer_texture` in
`src/hle/hle_d3d8_texture.c`), the log confirms both textures are found and
filled, **and the brightness does not change**. So the quads' output does not
depend on their texture. Replaying with `--no-combiners` gives (26, 28, 33),
so the register combiners are not the whole story either.

**Where to look next.** What those three draws compute, and how they blend.
They use pixel shader token 2 and SRCALPHA/INVSRCALPHA with vertex colour
0x7F7F7F at alphas 0x34, 0x3F and 0x19. Work out what the combiner program
for token 2 should produce, and compare it with what `d3d8_combiners.c`
generates for it. The screen-copy work is groundwork and correct on its own
terms; it is not the fix.

## 2. A grey line across part of the screen

Reported from a play session: part of the screen is darker than the rest, with
a visible line between them, where xemu is evenly lit. **Not reproduced.** A
scan of every frame available for a straight brightness step found nothing
inside the image — the only sharp discontinuity is in the outermost one or two
pixels at the frame's edge.

This needs one F11 capture taken from the spot where it is visible. With that,
bisecting to the draw is quick: `--draws N` on the capture, the same method
that found draw 159 in half an hour.

The likely candidate, if it is real, is one of the three full-screen quads
from issue 1 not covering the whole screen, which would make these one bug
rather than two.

## 3. Controller bindings reported as not applying

A play session reported that bindings did not apply correctly, without saying
which controls. The runtime does load the saved config and does read pad 0 on
port 1, so the failure is in what a specific control does, not in the config
being ignored. **Needs the detail before it can be chased.**

One real defect found while looking: the default keyboard mapping leaves all
eight stick directions unbound (`DEFAULTS` in `src/input/input_bindings.c` has
`NULL` for `lstick_*` and `rstick_*`), so on a keyboard you cannot move or
look at all. This is pre-existing rather than a regression — the old hard-coded
mapping had no stick keys either — but it makes the keyboard useless for a
first-person game. Fixing it means moving the face buttons off W, A and S,
which changes behaviour people may be used to, so it is a decision rather than
a patch.

## 4. Performance items 8 to 13

`docs/technical/ts2-performance-plan.md` items 1 to 7 are done and took the
level from 12.4 ms a frame to 5.0 ms. What is left, in the order the old
profile suggested:

- **9** — cheaper texture change detection. `level0_checksum` was ~4% of the
  guest thread.
- **8** — hash-index the texture cache instead of a linear scan of 512 entries
  per `SetTexture`.
- **10** — hoist `strlen` out of the trace hook's hot path. Moot for a title
  lifted without `--trace-all-entries`, which both now are.
- **12** — make `nv2a_ack_thread` sleep rather than spin. Measured as worth
  nothing on a 20-core machine; it costs a core on a smaller one.

**Take a fresh `RECOMP_SAMPLE` profile before starting any of them.** The frame
is 5 ms now and its shape has changed; the old ranking was taken at 21 ms.

## 5. Smaller things

- **Cutscene audio** is fixed and measured, but only measured. The gap between
  the game's idea of the music and the music itself went from 42 seconds over
  two minutes to 10 ms. Nobody has sat and listened to a cutscene since.
- **The scripted path is not a save state.** `RECOMP_INPUT_SEQ` reaches Siberia
  in about 75 seconds only when the save folder holds no profile, because the
  game writes one at t≈40 s and the menu path then differs. Clear it by moving
  the folders aside, never by deleting them: real profiles live there.
- **In-level input is ignored in the state the script reaches.** The pad is
  read (confirmed with `RECOMP_INPUT_LOG=1`: the stick arrives as 32767) and
  the camera does not move, so the run sits in a state that ignores movement,
  apparently the level's opening cutscene. This is why walking to a reported
  spot has to be done by hand.

---

## What is worth knowing before touching any of it

- **Reproduce from a capture, not from a run.** `d3d8_replay` plays one frame's
  host calls back with no game running, so `--draws N` bisects a frame in
  minutes. Both bugs above were characterised that way.
- **F11 in a running build saves the frame on screen**, as a picture and as a
  replayable capture, numbered so several presses all survive. That is the
  fastest route from "it looks wrong here" to a capture somebody can bisect.
- **Measure brightness, do not eyeball it.** Two of the wrong turns above came
  from reading a screenshot by eye: the "tan planks" are ordinary level
  geometry that xemu draws in the same place, and a pixel detector tuned to
  them found a global colour grade instead.
