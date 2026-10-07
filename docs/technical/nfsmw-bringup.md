# Need for Speed: Most Wanted, XDK 5849

Brought up 7 October 2026 on `main` 63f18f0. Result: the title boots,
plays its VP6 movies (EA logo, the Mia intro, the race montage), runs the
language select, title screen, alias creation and save, and the career's
prologue race: driving, overtaking, collisions and the HUD, drawn with
the title's own colour grading. It presents at a locked 60 frames per
second, with each race frame taking about 4.5 ms of the 16.7 ms budget on
the development machine. All of this was done with scripted input; nobody
has played it with a pad yet. Free roam after the prologue has not been
reached.

## What it is

| | |
| --- | --- |
| `default.xbe` | 4,739,072 bytes, SHA-1 d359b482ae5029aa0e05171e283ed780bc8b9a4c (media-patched; `default.bak` beside it is the original, differing in 3 header bytes at file offset 0x220 and 4 bytes at 0x38B7B1) |
| Title ID | 0x4541007B |
| Entry point | 0x002B3C26 |
| XDK | 5849, plain D3D8 (not LTCG); XONLINES, XVOICE, XVOCREC linked |
| Lift | 26,265 functions, `--game-only`; 57 of 89 HLE replacements bind |
| Seeds | `config/seeds/4541007B.json`, 12 entries |

Data is `NFS\ZDIR.BIN` plus `ZZDATA0-17.BIN`; there are no demand-loaded
code or data sections worth a `config/sections` entry.

## Building it

```bash
py -3 -m tools.xdk_symbols "games/Need For Speed - Most Wanted/default.xbe"
py -3 scripts/recompile.py "games/Need For Speed - Most Wanted/default.xbe" \
    --work-dir games/_pipeline/nfsmw/out --project titles/nfsmw
cmake -S titles/nfsmw -B titles/nfsmw/build -G "Visual Studio 17 2022" -A x64
cmake --build titles/nfsmw/build --config Release --parallel
```

## Getting to the race unattended

The script below reaches the prologue race in about two minutes from a
fresh save folder. The down/A/Start loop answers "create a new alias?"
with Yes, accepts the default name on the keyboard (Start is Done) and
starts the career; `rt` is the accelerator.

```bash
SEQ="4000:a,22000:start,30000:start,36000:start,$(py -3 -c "print(','.join(f'{t*1000}:down,{t*1000+1200}:a,{t*1000+2600}:start' for t in range(44,96,4)))"),110000:a,114000:a,122000:rt:45000"
RECOMP_MUTE=1 RECOMP_SAVE_DIR=<fresh folder> RECOMP_INPUT_SEQ="$SEQ" titles/nfsmw/build/Release/nfsmw_recomp.exe
```

The alias is saved through `\Device\Harddisk0\partition1\UDATA`, which
the path layer maps into the **game folder**, not `RECOMP_SAVE_DIR` (see
"Not done"). Delete `UDATA` and `TDATA` there between scripted runs, or
the second run meets "NAME has been loaded" instead of the alias prompt.

## What it needed

In the order they stopped it.

1. **Seeds.** Twelve functions are reached only through pointers: worker
   start contexts, a vtable target in the movie path, and the VP6
   decoder's DSP routines. The decoder stores its routines in a table at
   0x004EE180 from a setup function at 0x002CDDxx, two of which
   (0x002D013E, 0x002D0A84) had never been called by the time of the run
   that found the others, so they were seeded from the table rather than
   from a log. Without them the intro movies decode as green blocks.
2. **A race in the guest heap** (`xbox_memory_layout.c`). The heap's and
   the contiguous arena's block tables had no lock, and the guest lock is
   off in the shipping configuration, so two threads in kernel bridges
   could edit them at once. A worker's TIB was then zero-filled by a
   second allocation over the same block. The fault was XAPI's
   `SetLastError` reading `fs:[4] + index*4` at guest 0xFFFFFF6C, one
   run in three. Both tables are now behind one SRW lock.
3. **The start-up utility-drive clear deleted the save folder**
   (`kernel_file.c`, `kernel_path.c`). MW clears the utility drive as
   XAPI does, twice: it opens `\Device\Harddisk0\Partition5\` and `Z:\`
   (a link it makes to Partition5) as directories, lists them and deletes
   every entry. The path layer mapped both to `Partition5.img`, and
   `NtCreateFile` answered a directory open of an image with the folder
   holding it: the save root. The title deleted every partition image,
   `TitleData`, `Cache` and the folder itself. With the default save
   location that is every title's saves. Two changes:
   - A partition's root (`PartitionN\`, or any path that resolves to the
     image with a trailing separator) now opens that partition's own
     folder from the rules table: Partition5 goes to `Cache\<title id>`,
     the same place `Z:\` goes. Partition 0 has no folder and keeps the
     old answer, which only volume queries use.
   - Deleting the save root, the game folder or a `PartitionN.img` is
     refused (`STATUS_ACCESS_DENIED`) and logged, through both
     `NtDeleteFile` and delete-on-close, whatever path led there.
4. **The race was flat grey** (`d3d8_shaders.c`). The colour-grading
   pass's final combiner is `fog.a * grey + (1 - fog.a) * graded scene`,
   with `fc0` = 0x807B7B7D. It draws an XYZRHW quad with fog on and no
   specular colour. For pre-transformed vertices, D3D (and the NV2A under
   the XDK's fog setup) takes the fog factor from specular alpha, which
   is 0 when the format has none. The fixed-function vertex shader used a
   constant 1. It now takes specular alpha when fog is on and table fog
   is off.
5. **A seam through the graded frame** (`d3d8_shaders.c`,
   `d3d8_device.c`). Pre-transformed positions were always normalised by
   the guest screen size. A 320x240 quad drawn into a 320x240 render
   target therefore filled only its top-left quarter, and MW's blur and
   accumulation passes are such quads. They are now measured against the
   bound render target's size; the screen keeps the guest size, so
   resolution scaling is unaffected.

## Tools added on the way

- **Captures record screen copies** (`D3D8CAP_SCREEN_COPY`, capture
  format 8; readers still take 5-7). A replayed frame used to show the
  title's frame-buffer textures as their snapshot (black, or a frame
  old), so MW's grading drew grey in replay for a different reason than
  it did live. A capture now replays the copy at the same point in the
  frame, and `--list-draws` prints `[screen copy after N draws]`.
- **`[HLE-D3D8] shadow palettes`** now also reports `SetPalette` calls
  and how many changed the palette. MW makes about 3,300 a second during
  a race, and every P8 texture keeps one palette, which is why the
  "variants" and "switches" counters stay at 0. That is correct
  behaviour, not a missed palette.
- **`[FILE]` lines** for a partition root redirect, a refused delete, a
  directory removal, and the host path of every delete-on-close.

## How the grey frame was found

This is worth repeating on the next title that draws a flat colour.
Capture a frame (`RECOMP_D3D8_CAPTURE`, `_SWAP`) and replay it with
`--draws N` across the frame: the scene was complete at draw 579 and
draw 601 turned it grey. `RECOMP_D3D8_PS_DUMP` on the replay printed
that draw's generated HLSL, where the final combiner's A input was the
fog register. `RECOMP_D3D8_PS_SHOW` is less useful here than it looks:
it applies to every draw, so in a title that copies its own frame the
copy already holds the altered image.

## Not done

- **Saves made through the device path ignore `RECOMP_SAVE_DIR`.**
  `\Device\Harddisk0\Partition1\UDATA\...` maps into the game folder,
  while `U:\` maps into the save folder. A scripted run is therefore not
  isolated, and a player's MW saves live beside the game. This is
  toolkit-wide, and moving it changes where existing titles' saves are,
  so it is left for its own change.
- **P8 textures replay through the wrong palette.** The capture records
  the four stage palettes at the start of the frame, but live, each P8
  texture is baked through its own palette when first drawn. MW's sky
  replays as stripes and is correct live.
- Free roam (Rockport after the prologue), pursuits, the garage, and the
  Xbox Live menus are untested. Sound has only run muted.
- The title presents every vblank. Nobody has checked whether the
  original console build runs its race at 30 and its simulation assumes
  it. The race clock advances 4.00 s per 240 presents, so game time
  matches real time here.
