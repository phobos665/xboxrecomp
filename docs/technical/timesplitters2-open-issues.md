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
and got black. That theory was recorded as dead: the title makes a texture
whose texels *are* the frame buffer, that is now recognised and filled with
the host's own frame (`src/d3d/d3d8_screencopy.c`, `framebuffer_texture` in
`src/hle/hle_d3d8_texture.c`), the log confirms both textures are found and
filled, **and the brightness does not change**. Replaying with
`--no-combiners` gives (26, 28, 33), so the register combiners are not the
whole story either.

---

### Corrected, 20 September 2026: not a colour grade, and not the quads either

**It is motion-dependent.** From a pad session: standing still the picture is
right, and it darkens as soon as the camera or the player moves. That single
observation reframes everything above, because it says what *correct* looks
like.

The quads' shader was identified properly rather than guessed (see
`RECOMP_D3D8_PS_DUMP=<n>` and the `binding shader <hash>` line it now prints;
the previous guess picked the wrong shader, because the dump order is the
order shaders are *compiled*). For TimeSplitters 2 it is `6C0A8FCA`, and it
computes

```
result.rgb = saturate(2 * v0.rgb * t0.rgb)   /* v0 = 0x7F7F7F, so ~ t0 */
result.a   = 2 * v0.a                        /* the blend weight */
```

With SRCALPHA/INVSRCALPHA that is `out = t0*a + dst*(1-a)`. **If `t0` is the
screen, `out = dst` exactly.** The pass blends the screen back over itself and
is a no-op while nothing moves; it only shows when the copy stops matching the
screen, which is to say it is a motion blur. The combiner program is right.

**Do not measure these draws in a replay.** `src/replay` never calls
`xbox_D3D8CopyBackBufferToTexture` — the copy is done in `src/hle` — so a
replay samples the captured guest texels, which are zeros. Every replay A/B of
these three draws is therefore measuring `t0 = black`, which the running title
does not have. That includes the 0.38 factor quoted above, and a later
measurement of 36.7% of the light kept at alpha 0x19 against 43.2% at 0x0A:
real numbers, but numbers about a black source texture, not about the game.

**Live, the copy arrives.** `RECOMP_HLE_D3D8_FB_PROBE=<n>` reads the texture
back every n swaps (`framebuffer_probe` in `src/hle/hle_d3d8_texture.c`). For
TimeSplitters 2 it reports mean 85–128 of 255 with 1183–1192 of 1200 samples
non-zero, once past the black loading screens. So `t0` is the screen, the
quads are close to neutral, and the screen-copy path is working.

That kills the obvious reading of this bug — that the quads blend black over
the picture — and it kills it twice over, because the first version of that
probe appeared to confirm it. That version called `IDirect3DTexture8::LockRect`,
which returns `tex->sys_mem`, the upload shadow; the copy writes the GPU
resource through a render target view and never touches it, so it reads zeros
either way. Read a readback path before believing a readback.

**So what is still unexplained.** The motion dependence is reported from a pad
session and is not in doubt, but no mechanism for it survives yet. The quads
sample the current frame, not the previous one, which makes them a no-op rather
than the motion blur they are presumably meant to be — so a fair guess is that
they are not the cause of the brightness change at all, and something else that
correlates with movement is. The next measurement is the quads' *live*
contribution: draw a frame with them and without, still and moving, from the
same viewpoint. Nothing about this should be concluded from a capture.

**Screenshots need the scene held constant.** The first still/moving pair
collected for this was taken standing inside a tunnel versus out in the open,
so the whole-frame means differed by 30–90% on scene content alone and said
nothing about motion. A usable pair is the same view, one still and one moving;
failing that, measure something at a fixed screen position, such as the weapon
HUD.

**Two claims above that no longer hold.** The colour cast is not gone: both
frames from that pad session measure B > G > R on snow. And the headline "about
a third of xemu's brightness" is a figure taken while moving; standing still it
is much closer.

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

  **Re-test this (20 Sep 2026).** A lifter bug found while chasing the right
  stick fits this description better than a cutscene does: `movsx r32, bp` was
  lifted as a zero extension, and this title's pad handler is the one place
  that reads right-stick X through `bp`. A zero-extended axis saturates and
  defeats the title's own deadzone, which looks exactly like "the pad is read
  and the camera does not move". The 32767 in the note above was read from the
  binding layer, which was never wrong -- the sign was dropped afterwards, in
  the title's own lifted code, where no input log can see it. Fixed and
  re-lifted; nobody has re-run the scripted path since.

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
