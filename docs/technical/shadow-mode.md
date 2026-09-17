# Shadow mode: drawing the title through `src/d3d`

`RECOMP_HLE_D3D8=shadow` makes a recompiled title draw. Each replaced XDK
function runs the title's own body first (`HLE_ORIGINAL`), then repeats the
call on a host device (`src/d3d`, Direct3D 11) in a second window. The title's
own frame buffer is untouched, so the two can be compared.

Burnout 2 renders its logos, menus, loading screens, HUD and race this way.

---

## The layers

```
title (XDK D3D8, statically linked)
  |  40 functions replaced by name (tools/recomp/hle.py, HLE_EXPORT)
src/hle          Xbox -> host conversion
  |  host_* wrappers, one per host call
src/hle/hle_d3d8_record.c        <- the seam
  |                        \
src/d3d (D3D11)             .d3dcap capture file
                                  |
                            src/replay/d3d8_replay.c
```

**Nothing in `src/hle` calls a device vtable entry or a `d3d8_vsh_*` /
`d3d8_combiners_*` setter directly.** A direct call is one the capture cannot
see, and a replay that silently differs from the run. The single exception is
reading the back buffer for frame dumps, which changes no state.

## What reaches the host, and from where

| Host state | Source |
|---|---|
| Clear, Swap, viewport, transforms | the replaced call's arguments |
| Render and texture stage states | the title's own D3D8 state arrays, read by name (`HLE_IMPORT_VAR`, XDK 4627+ layout only) |
| Vertex programs | the `X_D3DVertexShader` object: microcode plus its parsed declaration |
| Vertex data (UP and buffer draws) | guest memory, with NORMPACKED3 expanded on the CPU |
| Textures | the guest pixel container, cached by VA + data + format + size |
| **Pixel shaders** | the `D3DPIXELSHADERDEF` inside the object `SetPixelShader` selects (at +0x0C, with a pointer to it at +0x08) |
| Combiner constants | the render state array (`SetPixelShaderConstant` is assumed to land there; a mismatch with the definition's own C0 is logged once) |
| Render targets | the Xbox surface's parent: a texture's level 0 becomes a host render target texture, the frame buffer becomes the back buffer, anything else a scratch target |

The frame buffer and the automatic depth buffer are recognised by identity:
they are the surfaces the XDK itself passes to `SetRenderTarget` inside
`CreateDevice`. Recognising them by size alone sends a title's full-screen
offscreen passes to the screen.

## Frame capture and replay

A capture records every host call for one frame, plus the vertex, index and
texture bytes those calls reference, and opens with a snapshot of the host's
state read back from `src/d3d`. Replay plays it into the same renderer with no
game running, no guest memory and no recompiled title.

That is the project's fast loop: **change the translation, replay the same
frame, compare the images.** A capture is deterministic, where two runs of the
title never land on the same moment.

```
# take captures of the frames that actually draw a scene
RECOMP_D3D8_CAPTURE=caps/c.d3dcap RECOMP_D3D8_CAPTURE_SWAP=2000 \
RECOMP_D3D8_CAPTURE_EVERY=150 RECOMP_D3D8_CAPTURE_MINDRAWS=300 your_game_recomp.exe

# replay one, both ways, and look at the two images
d3d8_replay caps/c_02000.d3dcap --out with_
d3d8_replay caps/c_02000.d3dcap --out without_ --no-combiners

# find which draw is at fault
d3d8_replay caps/c_02000.d3dcap --list-draws        # every draw, its state, targets, clears
d3d8_replay caps/c_02000.d3dcap --draws 690         # stop after 690 draws
d3d8_replay caps/c_02000.d3dcap --skip-draw 703     # leave one out
```

Captures hold the title's own textures and vertices, so **a capture is game
content and is never committed** — `.gitignore` covers `*.d3dcap`.

`tests/d3d8_capture` round-trips a synthetic capture (no game data) and runs
on Windows and Linux.

## Switches

| Switch | Effect |
|---|---|
| `RECOMP_HLE_D3D8=shadow` | draw the title through `src/d3d` in a second window |
| `RECOMP_HLE_D3D8_PS=0` | stop forwarding pixel shaders; use the host's fixed-function pixel path |
| `RECOMP_HLE_D3D8_PS_PROBE=1` | dump the first shader objects `SetPixelShader` is given |
| `RECOMP_HLE_D3D8_DUMP=<prefix>` | write the host back buffer as BMPs (at most 24) |
| `RECOMP_HLE_D3D8_DUMP_EVERY=<n>` | swaps between dumps |
| `RECOMP_HLE_D3D8_DUMP_MINDRAWS=<n>` | dump only frames with at least n draws |
| `RECOMP_D3D8_CAPTURE=<path>` | record a frame (see above) |
| `RECOMP_D3D8_CAPTURE_SWAP`, `_EVERY`, `_MINDRAWS` | which frames to record |
| `RECOMP_D3D8_PS_SHOW=<reg>` | draw one combiner register instead of the result: `v0`, `t0`, `r0`, `fog`, or `<reg>a` for its alpha |
| `RECOMP_D3D8_PS_DUMP=1`, `RECOMP_D3D8_VS_DUMP=1` | print the generated HLSL |

`MINDRAWS` matters more than it sounds: a title's 3D frames can be rare among
its 2D ones. Burnout 2 draws 2-6 times on a loading frame and 690-910 in a
race frame, so a fixed interval samples loading screens almost every time.

## What is per-title here

Very little. The replacement boundary is per XDK build, and below it everything
is NV2A behaviour that every title shares — shader translation, texture
formats, render target rules. What is per-title is which XDK the signature
database has to identify, the discovery seeds, and titles that fill their own
push buffers through `BeginPush`, whose draws no replacement ever sees.

## Known gaps

- Cube render targets go to scratch targets, and cube textures bind white. A
  title's environment reflections are therefore missing.
- `CopyRects` is not handled.
- `SetVertexData4f` / `SetVertexData4ub` are not replaced; `SetVertexDataColor`
  and `SetVertexData2f` are.
- A render target texture replays as zeros: its contents are drawn, never in
  the guest's memory, so a capture that samples one without drawing into it
  first shows nothing.
- Lights and material are not forwarded, so lighting stays off.
- Only the XDK 4627+ render state layout is read; anything else turns state
  forwarding off and says so.
