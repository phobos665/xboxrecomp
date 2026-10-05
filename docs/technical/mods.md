# Mods: the overlay folder and patch files

Two runtime features, for every title, that let a mod change a game without
editing its disc or rebuilding it. Added October 2026 on `feat/ts2-disassembly`;
TimeSplitters 2 is the first user (split2-recomp `docs/modding.md`).

| | Changes | Code | Test |
| --- | --- | --- | --- |
| Mods folder | files the title opens from the disc | `src/kernel/kernel_path.c` | `tests/kernel_path_mods` |
| Patch files | values in the title's memory before it starts | `src/kernel/mod_patches.c` | `tests/mod_patches` |
| Override folder | which functions are hand-written (build time) | `tools/recomp/manual_scan.py` | `tools/recomp/test_manual_scan.py` |

## The mods folder

`<mods>/<path>` replaces `<game_dir>/<path>` for every disc path (`D:\`,
`\Device\CdRom0\`, `\??\D:\`). The folder is `RECOMP_MODS_DIR` (or `mods_dir`
in the title's settings file), else `mods` beside the executable. `off`,
`0` or `none` switches it off, and it is used only if it exists. Start-up says
which folder it found (`[MODS] overlay ...`), and each open it answers logs
`[MODS] <xbox path> -> <host path>`.

Deliberately narrow:

- **Disc paths only.** `\Device\Harddisk0\Partition1\` shares the game
  directory in this runtime and holds saves (TimeSplitters 2's `UDATA`), so it
  is never overlaid. Neither are T:, U:, Z: or the cache partitions.
- **Files only.** A directory in the mods folder does not replace one on the
  disc, and `NtQueryDirectoryFile` lists the disc's directory. A mod replaces
  files a title opens by name. Merging listings is possible later if a title
  needs a mod to *add* files it discovers by listing.
- **Case.** Windows matches names case-insensitively as the console did. On
  Linux the overlay is as case-sensitive as the rest of the path layer.

The path the overlay chose is what `xbox_LastHostPath()` returns, so anything
that acts on the file a title just opened (the FMV hook) sees the mod's file.

## Patch files

Every `.json` in `<mods>/patches`, in name order, applied once at the end of
`xbox_kernel_bridge_init`. That is the last start-up step every host shares
before guest code runs, so no title's `main.c` needed to change (and
watchpoints arm after it, so patching does not trip them). The format is in
`src/kernel/mod_patches.h`:

```json
{ "title": "4553000A",
  "patches": [ { "address": "0x0025A0F0", "type": "f32",
                 "expect": 1.0, "value": 1.5, "note": "..." } ] }
```

The rules that matter:

- **`expect` guards.** If it is given and does not match, nothing is
  written and the log prints what was found, so a patch written for another
  release does nothing instead of writing over something else. An array is
  all or nothing.
- **`title` filters.** A file for another title id is ignored without fuss,
  so one mods folder can serve several games.
- **Data, not code.** Lifted code is compiled C and never reads its own
  instructions, so a patch into a section the XBE marks executable writes and
  warns. Behaviour belongs in an override.
- **Start-up only.** Every XBE section is resident from the start here
  (`XeLoadSection` is bookkeeping), so a patch to the image stays. A table a
  title builds or loads from a file later is not there yet; replace the file.

The JSON reader is a small one of its own, as in `src/input/input_bindings.c`;
the runtime has no JSON library.

## The override folder

`--exclude-manual` takes a directory as well as a file, and may be repeated.
A directory stands for every `*.c` beneath it, in sorted order.
`scripts/recompile.py` and `compat_matrix.py` pass a title's
`src/overrides/` beside its `recomp_manual.c` when it exists. A `sub_`
defined in two override files is reported by name at lift time
(`duplicate_definitions`) instead of as a duplicate symbol at link. A title
that adopts the folder must glob it in its CMakeLists (split2-recomp does).
`recomp_manual.c` stays the home of `recomp_lookup_manual` and the template's
diagnostics.

## What is not here yet

- The hashed texture dump and replace of `modding-models-textures.md` (T4,
  T5). The overlay is its Route A: a rebuilt archive in the mods folder.
- Audio and mesh replacement at the renderer and DirectSound boundaries.
- A launcher row for the mods folder; today it is the environment variable
  or `mods_dir` in the settings file.
