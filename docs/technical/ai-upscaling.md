# AI texture, font and UI upscaling

How AI-upscaled texture packs work, what this toolkit needs before it can load
one, and the workflow for making one. Written Oct 2026 as a hand-off: part 1 is
background, part 2 is the runtime work for whoever implements it (an agent or a
person), part 3 is the offline scripts, part 4 is the workflow for the person
making a pack.

Nothing here is built yet. Every code reference was checked against `main` at
05040ec.

---

## 1. How it works

**The AI model never runs while the game does.** A texture pack is an offline
batch job wrapped around two runtime hooks:

1. **Dump.** The renderer writes every texture the title binds to a folder,
   named by a hash of its original bytes.
2. **Upscale offline.** A super-resolution model (Real-ESRGAN, or one of the
   community models below) turns each image into a 4x one. These models invent
   plausible detail rather than recovering it, which is why they beat bicubic
   and also why they sometimes hallucinate.
3. **Re-encode.** Back to a GPU format (BC1/BC3 DDS) with a full mip chain.
4. **Replace.** When the title loads a texture, the runtime hashes it; if
   `<hash>.dds` is in the replacement folder, the host texture is built from
   that file instead of from guest memory.

The title never knows. It samples with 0..1 coordinates, so a 1024x1024
replacement covers the same surface as the original 256x256. The content hash
is what makes this work for any title with no knowledge of its file formats:
nobody needs to know what is inside a Burnout pak, only to recognise the same
bytes when they are loaded again.

This is the same mechanism Dolphin and PCSX2 texture packs use, and the
community tools are the same.

### What a pack can and cannot change

| Changes | Does not change |
|---|---|
| World, car, character textures | Render targets: reflections, blur, shadows, anything the title draws into |
| UI art, logos, HUD pieces | The frame buffer bound as a texture |
| Font atlases (redrawn, not upscaled; see 4.4) | Geometry |
| | Textures streamed or rewritten at run time (movies, scrolling sprite sheets) |
| | Lookup tables and gradients the title uses as data |

Render targets get sharper from internal resolution (`RECOMP_RES_SCALE`), not
from a pack. The two are complementary: resolution scaling makes edges sharp
and leaves textures blurry; a pack fixes the textures.

### "Someone uses Real-ESRGAN for Burnout 3"

Not verifiable from here. Burnout 3's recompilation is its own fork with its own
runtime (`docs/technical/burnout3-reunification.md`); it is not one of the
titles in `titles/`. What the phrase almost certainly describes is the loop
above, done in that fork or in an emulator, not a real-time AI upscaler.

---

## 2. Runtime work (for the implementer)

### 2.1 Where things stand

- **The HLE renderer has no dump and no replacement path.**
  `docs/technical/modding-models-textures.md` designs one (items T4, T5, T7)
  and names the hook point: `host_texture()` in `src/hle/hle_d3d8_texture.c`,
  between `read_layout()` and `upload()`.
- **A dump exists only in the LLE push-buffer executor**
  (`RECOMP_TEX_DUMP` in `src/kernel/nv2a_pb_exec.c:1086`). It writes BMPs named
  by GPU offset, so it cannot key a replacement. **The name `RECOMP_TEX_DUMP` is
  taken**: the modding doc proposes reusing it, which would collide. Use
  `RECOMP_HLE_D3D8_TEX_DUMP` and `RECOMP_HLE_D3D8_TEX_REPLACE`, matching the
  other `RECOMP_HLE_D3D8_TEX_*` switches in that file.
- Replacement only reaches what the HLE renderer draws (`RECOMP_HLE_D3D8`, on
  by default). Draws through a hand-filled push buffer (`BeginPush`; Burnout 2
  has two such call sites) bypass it, and so do their textures.

### 2.2 The cache, as it is

`host_texture()` (`src/hle/hle_d3d8_texture.c:621`) looks a texture up by
`(va, data, format, size)` in a 512-entry cache (`TEXTURE_CACHE`). Before the
lookup it diverts the frame buffer (`framebuffer_texture`) and memory the title
rendered into (`rendered_surface_for`). On a miss it calls `read_layout()`,
creates a `D3DPOOL_MANAGED` host texture **in the guest's own Xbox format, size
and level count**, and `upload()` copies the guest bytes; the host D3D8 layer
unswizzles and converts at upload (`d3d8_upload_mip_level`,
`src/d3d/d3d8_resources.c:1442`). On a hit it re-checks the texels once a frame
(`texels_changed`, tiered sample-then-full hash) and re-uploads when they moved.

Facts that shape the replacement:

- **The existing checksum is not a usable key.** `level0_checksum()` folds a
  64-bit hash to 32 bits, has a sampled mode behind
  `RECOMP_HLE_D3D8_TEX_SAMPLED`, and exists to detect change, so it is free to
  change. A pack key is a file format: it has to stay the same forever.
- **Swizzled uncompressed formats are unswizzled at upload.** Creating a host
  texture as `D3DFMT_A8R8G8B8` and feeding it linear BGRA gives scrambled
  pixels. DXT1/3/5 are neither swizzled nor linear, so they upload as they are.
  `xbox_swizzle_rect()` exists in `src/d3d/d3d8_swizzle.h:191` if an
  uncompressed replacement is ever needed.
- **Linear (`LIN_*`, YUY2) textures are addressed in texels.** The pixel shader
  scales their coordinates by `1 / host texture size`
  (`d3d8_combiners.c:1555`, via `d3d8_base_size`). A 4x replacement makes
  that scale 4x too small and the title samples only the top-left quarter.
- **P8 textures are expanded per palette** (`sync_palette`, one host texture
  per palette a sheet is drawn with). A replacement keyed on the indices alone
  would lose palette swaps, which is how Marvel vs Capcom 2 colours its
  fighters.
- **A texture can become a render target later.** `hle_d3d8_render_texture()`
  (line 1234) recreates a cached ordinary texture as a render target at the
  guest size, so it drops a replacement naturally, but any `replaced` flag has
  to be cleared there.
- **The host formats are BC1, BC2, BC3 and BC5 only** (`RHI_FORMAT_*` in
  `src/d3d/rhi.h`). There is no BC7, and D3D8 has no enumerator for it.

### 2.3 Work items

In order. The first three are a usable v1.

**R1. The key (0.5 day).** A 64-bit hash over `t->bytes` of guest data (every
level) plus `fmt`, `width`, `height`, `levels` and `linear`. Write it as its own
function with a fixed algorithm (FNV-1a 64 or a vendored XXH64; either is
fine), a comment saying **changing it invalidates every pack**, and a ctest with
a fixed input and its expected value so nobody changes it by accident. Files
are named `<16 hex digits>.dds`. Compute it only when a cache entry is created
and when `texels_changed` reports new content, never per bind.

**R2. The dump, `RECOMP_HLE_D3D8_TEX_DUMP=<dir>` (1-1.5 days).** On every new
key, if `<dir>/<key>.dds` does not exist, write it and append a line to
`<dir>/manifest.jsonl`. Skip rendered, frame-buffer and rendered-surface
entries; they have no texels of their own.

- DDS content: compressed formats as their raw blocks (a DDS header plus the
  bytes, all levels); uncompressed formats unswizzled and converted to
  A8R8G8B8 with the code the upload already uses (`xbox_unswizzle_rect`,
  `d3d8_convert_linear_pixels`). P8 is dumped expanded through the palette it is
  drawn with, so for P8 the key covers indices plus palette (see R6). Writing
  the original format exactly keeps the dump lossless; the offline scripts
  decode it.
- Manifest fields: `key, fmt (name and code), width, height, levels, linear,
  p8, has_alpha (any texel below 255), first_swap, binds, dynamic`. `dynamic`
  is set when the same cache entry later produces a different key: those are
  streamed textures and the sorter skips them. `binds` lets someone prioritise
  what the player actually sees.
- Writing a file stalls the frame. Acceptable for a dump run; say so in the log
  (`[TEXDUMP] wrote N textures (M new this second)`), once a second at most.

**R3. The replacement, `RECOMP_HLE_D3D8_TEX_REPLACE=<dir>` (2-3 days).**

- At start-up, index `<dir>` (file names only) into a hash map. Log
  `[TEXREPLACE] <n> replacements in <dir>`.
- In the miss path of `host_texture()`, after `read_layout()` and before
  `host_CreateTexture`: compute the key; on a hit, load the DDS and create the
  host texture from it, at its own size and format, instead of from guest
  memory. Mark the entry `replaced`.
- v1 accepts DDS in DXT1, DXT3, DXT5 only (BC1/2/3), so the existing upload
  path takes it with no swizzle and no conversion. Refuse anything else with a
  once-per-file log line.
- v1 never replaces linear textures, P8 textures, or textures larger than 4096
  after scaling.
- **Guard 1, change:** when `texels_changed` fires on a `replaced` entry,
  release its host texture and rebuild the entry through the normal miss path
  (the new content may have a replacement of its own, or none, and a
  replacement-sized host texture cannot take the guest's bytes).
- **Guard 2, render target:** clear `replaced` in `hle_d3d8_render_texture()`
  when it recreates an entry.
- **Guard 3, eviction:** an evicted entry that comes back reloads from disk.
  Keep the index, not the bytes; reloading a DDS is fast enough, and if it is
  not, add a small byte cache later, measured.
- Memory: 4x in each dimension is 16x the texels. A level with 32 MB of guest
  textures can need ~512 MB of host memory at 4x even compressed. Log the
  running total of replaced bytes in the existing five-second texture line.

**R4. Linear textures (1 day).** Give `D3D8Texture` a logical size, defaulting
to its real size, and have `d3d8_base_size` (or a new accessor used only for
`tex_scale`) return it. A replacement of a linear texture sets the logical size
to the guest's. Then lift the v1 exclusion. Check with a title that draws UI
through linear textures before declaring it done.

**R5. BC7 (1-2 days).** `RHI_FORMAT_BC7_UNORM` in both backends
(`rhi_d3d11.c`, `rhi_vulkan.c`) and a private host format code for it, used
only by replacements. BC7 holds noticeably better than BC3 on upscaled
photographic textures. On macOS, check that MoltenVK on Apple Silicon exposes
BC7 before relying on it.

**R6. P8 (1 day).** Key P8 replacements on indices plus palette, so each
palette variant is its own replacement (and the dump writes each variant it
sees). The replacement entry is a plain BGRA/BC texture; it must not go through
`sync_palette` again.

**R7. Name by disc asset (per title, optional).** The modding doc's T6: an
offline indexer hashing a title's packed assets so the manifest can say
`chr.pak:textures/0267.xbt` instead of a hash. Toolkit hashing, title-specific
unpacking.

### 2.4 Rules that apply

- No ctest or build step may run a title (CLAUDE.md, macOS port). Test the hash
  and the DDS reader/writer as units; test dump and replace by hand.
- Scripted runs take `RECOMP_SAVE_DIR=<fresh dir> RECOMP_MUTE=1
  RECOMP_WINDOW_BACKGROUND=1`, from a copy of the game folder without `UDATA`
  (`scripts/regress.py` does this).
- `src/hle` is shared HLE: a change that alters what Windows draws needs a
  Windows regression of TS2 and BLiNX before `main`. With both switches unset,
  the code path must be exactly today's.
- Frame capture records host calls, so a replaced texture is captured as
  replaced. Capture with replacement off when reporting a renderer bug.

---

## 3. Offline scripts (for the implementer)

A `tools/texpack/` package, run as `python3 -m tools.texpack <command>`.
Python with Pillow (it reads DXT1/3/5 DDS) for decoding; `texconv`
(DirectXTex) or Compressonator for BC encoding, called as an external
program. None of it needs a GPU; the upscaler runs separately (section 4).

| Command | Does |
|---|---|
| `decode <dump>` | DDS to PNG (RGBA8, top level only), same key names, into `<dump>/png/` |
| `sort <dump>` | Splits the PNGs into category folders from the manifest and simple image checks (table below), writes `sort.csv` saying why each landed where it did |
| `pad <dir>` / `crop <dir>` | Wrap-pad tiling textures by N pixels before upscaling, crop the scaled padding off after |
| `encode <dir> <out>` | PNG to DDS with a full mip chain: BC1 without alpha, BC3 with alpha (BC7 once R5 lands); refuses to grow a texture past 4096 |
| `sheet <orig> <new>` | Contact sheet: an HTML page of before/after thumbnails grouped by category, with the key under each so a bad one can be found |
| `check <pack>` | Every file is a valid DDS in an accepted format, power-of-two, same aspect ratio as its original, name is a key the manifest knows |

### Categories for `sort`

| Folder | Rule | Treatment |
|---|---|---|
| `skip/` | `dynamic`, linear (until R4), P8 (until R6), either side ≤ 16 px, or one of width/height is 1 (a ramp) | Not upscaled |
| `fonts/` | Mostly transparent, high-contrast glyph shapes on a grid; confirm by eye, the rule only proposes | Redrawn by hand |
| `alpha_soft/` | Has alpha with many intermediate values (smoke, glow, shadow blobs) | Plain resize, or skip |
| `alpha_cutout/` | Has alpha that is mostly 0 or 255 (foliage, fences, decals) | RGB through the model, alpha resized separately |
| `tiling/` | Left/right and top/bottom edges match closely | Pad, upscale, crop |
| `ui/` | Few distinct colours, large flat areas | Illustration-trained model |
| `opaque/` | Everything else | Main model |

The rules will misfile some textures. The sorter's job is to get the first 90%
right and say why it chose, so the person moves the rest by hand.

---

## 4. Making a pack (for you)

You do not go through textures one by one. You decide the treatment once per
category on a small sample, let the tools apply it to the whole folder, and
then spend your time on the few that come out wrong.

### 4.1 The tools

| Tool | Use |
|---|---|
| **chaiNNer** | The standard for texture packs. Node-based GUI: build the chain once (split alpha, upscale, merge, save), point it at a folder. Free; runs on NVIDIA, AMD and Apple GPUs |
| **OpenModelDB** | Catalogue of community models, and it matters more than the tool. Stock Real-ESRGAN is tuned for photos and anime; look for game-texture models, illustration models for UI, and 1x "de-compression" models that clean DXT blocking before the upscale. 4x-UltraSharp and Remacri are well-known general models |
| **Real-ESRGAN-ncnn-vulkan** | Command-line Real-ESRGAN, one binary, any Vulkan GPU. For scripting without chaiNNer |
| **spandrel** | The Python library chaiNNer uses to load models; for a pure-Python pipeline |
| **texconv / Compressonator** | PNG to BC DDS with mips (`tools.texpack encode` calls one of them) |
| **Upscayl** | One-click GUI, fine for trying a model, no alpha handling or batch chain |

Claude cannot upscale images itself, and the cloud sessions have no GPU. The
upscale runs on your machine; everything around it is scripted.

### 4.2 Step by step

1. **Dump.** Run the title with `RECOMP_HLE_D3D8_TEX_DUMP=<dir>` (and a fresh
   `RECOMP_SAVE_DIR`) and play through everything you want covered: every track
   or level, every menu, every car or character. You only get textures you
   saw. Expect a few thousand files.
2. **Decode and sort.** `python3 -m tools.texpack decode <dir>`, then `sort`.
   Skim each category folder and move misfiles. Pull the font atlases out.
3. **Pick models (an afternoon).** Take 10-20 representative textures per
   category, run two or three candidate models on them in chaiNNer, compare
   side by side, pick a winner per category.
4. **Build a chain per category in chaiNNer**, for example `alpha_cutout`:

   ```
   Load Images (folder) -> Split Transparency -+- RGB   -> [1x de-block model] -> [4x model] -+
                                               +- Alpha -> Resize 4x (Lanczos) ---------------+-> Merge Transparency -> Save Image (PNG, same name)
   ```

   `opaque` and `ui` are the RGB branch alone. `tiling` runs
   `tools.texpack pad` before and `crop` after. Run each chain over its folder
   and leave it: minutes to a few hours depending on the GPU.
5. **Encode.** `python3 -m tools.texpack encode <upscaled> <pack>`.
6. **Review.** `python3 -m tools.texpack sheet` and scroll the page. Look for
   faces turned to plastic, invented patterns, text-like detail turned to mush,
   colour shifts. Expect a few percent to fail: re-run those through another
   model, leave them at the original (delete the file from the pack), or fix
   them by hand.
7. **Play.** Run with `RECOMP_HLE_D3D8_TEX_REPLACE=<pack>`. Some problems only
   show in context: a seam where two textures meet, a texture now much sharper
   than its neighbour, a decal that no longer lines up. Fix those one at a time.
   F11 captures the frame if you want to show someone.

### 4.3 Where your time goes

- Model picking: an afternoon.
- Contact-sheet review: an hour or two for a few thousand textures.
- Hand work: font atlases, the title logo, and what the player stares at (the
  car in a racer, the hands and weapon in a shooter). A good pack hand-touches
  dozens of textures, not thousands.

### 4.4 Things that go wrong

- **Alpha.** The models are RGB. Feeding RGBA in gives halos around foliage and
  fences; split it (4.2, step 4).
- **Seams.** A tiling texture upscaled on its own does not tile any more. Pad
  first.
- **DXT blocks.** A generic model sharpens 4x4 compression blocks into fake
  detail. Use a 1x de-blocking model first, or a model trained on compressed
  sources.
- **Fonts.** Upscalers wobble glyphs and melt serifs, and neighbouring glyphs
  bleed into each other in the atlas. Redraw the atlas at 4x keeping every glyph
  in the same relative position, or render a real font into the same layout.
  The title finds glyphs by normalised coordinates, so a bigger atlas works if
  the layout matches.
- **UI.** Photo models smear flat colour and outlines; use an illustration model.
- **Data textures.** Fog ramps, lighting lookups, noise tables: "enhancing" them
  breaks rendering. The sorter skips the obvious ones; anything that looks like
  a gradient or noise, leave alone.
- **Too sharp.** A 4x texture beside an untouched one looks worse than both at
  the original. Cover whole sets (a whole track, a whole car) rather than
  scattering replacements.

### 4.5 Distribution

A pack is derived from the game's textures, so it carries the same constraint
as everything else here (CLAUDE.md, Legal): do not distribute game assets. Keep
packs for your own use.

---

## 5. Open questions

- Whether the font atlases of the titles in `titles/` are textures at all, or
  drawn some other way (XFONT, a title's own glyph renderer into a render
  target). Check with the dump before planning font work for a title.
- How much of a title's art reaches the HLE renderer as opposed to `BeginPush`
  draws. A dump run's manifest against a frame capture would say.
- Whether a 4x pack fits in memory on the Steam Deck class of machine the Vulkan
  backend targets. Probably needs BC7 (R5) and possibly 2x packs there.
