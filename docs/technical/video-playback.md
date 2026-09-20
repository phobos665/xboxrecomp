# Replacing the video library

Done 20 September 2026, on `feat/video-hle`, with Black as the case.

A title that opens with a logo movie decodes it with XMV, the Xbox video
library. Like every SDK library it is linked into the XBE, so it is lifted and
runs as the title's own code, and it is some of the least forgiving code in
any image: hand-written MMX inner loops fed by pointers that come from the
graphics layer. Black faults inside one of them within seconds of starting,
having reached nothing else.

**Refusing to open the file does not help.** Black treats a missing movie as
fatal. It calls its own error callback, which asserts it is not on the main
thread and halts. That is reasonable behaviour, because on a real disc the
file is always there. `RECOMP_SKIP_VIDEO` is still right for a title that
merely moves on; Black is not one.

So the movie is replaced rather than decoded, at the library's API boundary.

## Finding the boundary without a signature database

The database this project uses covers Direct3D, DirectSound and the
application library. It does not cover the video library, so there were
addresses and no names.

`scripts/section_calls.py` finds them another way. Each SDK library gets its
own named section, so the address range already says which functions belong
to it. What it does not say is which are **entry points**, and that is the
only part that matters. The tool reads the generated C, where every call site
records both the function it is written inside and the address it calls, and
reports the calls that cross into the section from outside.

```
XMV spans 0x0023EB40..0x00266874
5 entry point(s), most-called first

  entry        args   via      called from
  0x0023F291 1      call     0x000C4560 0x00209D40
  0x0023EC4D 3      call     0x000D3620
  0x0023F191 1      call     0x000DB190
  0x0023F275 2      call     0x000D3620
  0x0023F45C 4      call     0x000C4570
```

**Five functions out of a 163 KB library.** Argument counts come from each
callee's own `ret N`, read out of the binary, so the signatures are the
binary's rather than a guess.

The tool works on any library. It reports 39 entry points for DirectSound in
the same title, shows three of the video entry points calling into it, which
is what a movie with sound looks like, and tells you plainly when a title has
no such section at all, as TimeSplitters 2 does.

## Naming them

Reading the callers is what turns five addresses into five functions.

| Address | Name | How the caller gives it away |
| --- | --- | --- |
| 0x0023EC4D | `XMVPlaybackCreate` | Third argument is the address of the caller's own handle field. A non-zero return makes the title fetch a callback and invoke it. |
| 0x0023F191 | `XMVPlaybackDestroy` | The caller zeroes its handle field immediately afterwards. |
| 0x0023F275 | `XMVPlaybackGetStreamInfo` | Nine instructions: copies the object's +0x40, +0x44 and +0x48 into the caller's struct at +0, +4 and +0xC. |
| 0x0023F291 | `XMVPlaybackStart` | The caller sets its own "playing" flag straight after. |
| 0x0023F45C | `XMVPlaybackUpdate` | Called once per pump with a surface and an out-parameter the caller then switches on. |

The switch is the important part. The caller reads the status, rejects
anything above 3, and jumps through a four-entry table:

```
status 0 -> counts a frame and keeps going
status 1 -> sets the title's "movie done" flag
status 2 -> paused
status 3 -> paused
```

So **status 1 is the transition the whole thing turns on**, and the title's
video state machine advances to its next state as soon as it sees one.

## Where the names live

`config/extra_symbols/<title id>.json`, merged by `tools.xdk_symbols` into the
file it generates. From there the existing replacement machinery does the
rest: `src/hle/hle_xmv.c` marks five functions with `HLE_EXPORT`, and the
recompiler generates a thunk from each address to each implementation. No new
binding mechanism was needed.

**Addresses are per title, not per SDK version.** A statically linked library
lands wherever the linker put it, so two titles on the same SDK have it in
different places. Only the behaviour is shared, which is why the
implementation is in `src/hle` and only the addresses are per title.

## What the replacement does

Answers the five calls without decoding anything. It hands back a playback
object with the dimensions and frame rate the title reads through
`GetStreamInfo`, reports the movie playing for the first few polls and
finished after that, and frees nothing because the guest heap has no free.

`RECOMP_XMV_SECONDS` gives the movie a real length, and the default is zero.
Zero is deliberate: nothing is drawn while the title waits, and the waiting is
not free. Black polls the playback in a tight loop rather than once a frame,
and a one-second movie cost it **17.8 million polls** of a burning core to sit
through a black screen. Three polls report the movie playing so a title that
wants to see a beginning sees one, and the fourth ends it.

`RECOMP_HLE_XMV=0` gives the title its own decoder back.

It draws nothing. With `RECOMP_FMV_HOST` the host's own decoder plays the file
when the title opens it, and this keeps the title's state machine happy
alongside it.

## Result on Black

| | before | after |
| --- | --- | --- |
| outcome | access violation in the decoder | no crash |
| swaps in 40 s | 3 | **2005** |
| draws | none | **10,173** |
| draws skipped by the renderer | — | **0** |
| vertex shaders translated | — | 27 |

The title gets through its intro, loads, and runs a frame loop at roughly 50
frames a second with every draw reaching the renderer.

## What it did not fix

**The picture is black.** Frames dumped with `RECOMP_HLE_D3D8_DUMP` are a
single colour at zero brightness across the whole frame. The capture path is
not at fault: the same switch on TimeSplitters 2 gives 800 distinct colours
and a mean brightness of about 50.

So Black issues ten thousand draws that produce nothing visible, which is a
rendering problem and not a video one.

**There is a concrete lead.** Black is the first D3D8LTCG title here, and its
symbols carry fifteen functions whose names record that the linker gave them
register-passed arguments:

```
D3DDevice_LoadVertexShader_4__LTCG_eax1
D3DDevice_SelectVertexShader_0__LTCG_eax1_ebx2
D3DDevice_SetPixelShaderConstant_4__LTCG_ecx1_eax3
D3D_SetTileNoWait_0__LTCG_eax1_ecx2
...
```

**None of the fifteen is replaced.** The replacement layer matches on the
plain name, so `D3DDevice_LoadVertexShader` never matches
`D3DDevice_LoadVertexShader_4__LTCG_eax1`, and those functions run as lifted
code with no replacement. They are the shader path, which is exactly what had
to be replaced to make TimeSplitters 2 draw.

**That is now done**, and it was not the cause.

`tools/recomp/hle.py` parses the suffix, matches a plain implementation name
against a single LTCG variant, and the generated thunk lays an ordinary
argument frame just below the stack, fills it from the registers the name
records and from the caller's stack for the rest, and points `g_esp` at it for
the call. Implementations are unchanged and cannot tell the difference. Two
more functions bind in Black as a result, including
`D3DDevice_SelectVertexShader`, taking it from 46 replaced to 48.

The picture is still black. Of the fifteen register-argument functions only
two have implementations at all, so the rest still run as lifted code, but
none of the thirteen is a pixel-shader entry point and `SetPixelShader` was
already bound in Black exactly as it is in TimeSplitters 2.

The real symptom is narrower than it looked:

```
[HLE-D3D8] shadow pixel shader: 0 draws with a combiner, 2594 without
```

**Not one of Black's draws carries a register combiner.** In TimeSplitters 2
the combiners are what produce colour. So the title either never sets a pixel
shader, or sets one the combiner layer declines to parse, and that is where
the next session should start: instrument `D3DDevice_SetPixelShader` to say
what it is handed and how often.
