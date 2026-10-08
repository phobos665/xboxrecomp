# Contributors

This project exists because people who care about the original Xbox keep showing up.
Thank you to everyone who has contributed code, fixes, testing or a hard-won debugging insight.
This file is the record of who did what. The [changelog](CHANGELOG.md) tells the story by date;
credit lives here.

Reporting a bug counts. Several people below never sent a patch and still moved the project
further than a patch would have, because they found the wall everyone else was about to hit.

If you have contributed and are not listed, or a line here is wrong, open a PR against this file.
We want every name right.

---

## Maintainer

### Phobos665 — [@phobos665](https://github.com/phobos665)
Maintains the project and leads its development. Since the fork point in September 2026:

- **The HLE layer and shadow renderer.** D3D8 replaced by name at the XDK boundary, with the
  title's own code still running underneath, and the shadow device that draws what the title
  draws. Frame capture and replay, and the cube-map, render-target and `CopyRects` work.
- **The renderer.** The Vulkan RHI and its MoltenVK workarounds, the SDL3 host layer, HLSL
  generation for register combiners and vertex programs, and the NV2A vertex-program encoding.
- **Platform ports.** The macOS arm64 port: the 4 GB base-offset memory model, the SIGBUS load/store
  emulator, the guest lock and its priority scheduling, and the lifter changes that make
  atomics, rounding and division faults behave the same on every host.
- **Enhancements.** Widescreen, internal resolution, adaptive frame cap, variable refresh rate,
  frame interpolation, the settings launcher, and rebindable input.
- **Title bring-up.** TimeSplitters 2 and Future Perfect, Burnout 2, Outrun 2, Mortal Kombat:
  Deadly Alliance (including its fiber scheduler), BLiNX, NFSU2 and WWE Raw 2, the projects scaffolded under
  `titles/`, and the toolkit fixes each one forced.
- **Tooling and diagnostics.** The kernel audit, the memory watchpoints, the exit trace, the
  regression and A/B harnesses, per-title function seeds, and the CI matrix across Windows,
  macOS and Linux.

---

## Contributors

### nemonicicon — [@nemonicicon](https://github.com/nemonicicon)
- **Halo: Combat Evolved and Hunter: The Reckoning (#35)** — two new titles brought up on the
  shadow renderer, the first on an XDK before 4034. Halo reaches its main menu, opening
  cinematic and first level, and Hunter plays its first level. The pull request also carried the
  toolkit fixes those two needed, most of which are XDK-version support rather than anything
  specific to either game. Write-ups: [Halo](docs/technical/halo-bringup.md),
  [Hunter](docs/technical/hunter-bringup.md).

---

## Inherited from upstream

This project began as a fork of [sp00nznet/xboxrecomp](https://github.com/sp00nznet/xboxrecomp)
and kept its history. The people below built what the fork started from. Their work is in the
disassembler, the lifter, the kernel bridge, the texture layer and the first runtime, and a
good deal of it is still how those parts behave today.

### sp00nznet — [@sp00nznet](https://github.com/sp00nznet)
Created the project. The x86 disassembler and lifter, XBE parsing, the kernel and XAPI runtime,
the first D3D8 and NV2A translation, the XISO and XMV tooling, the recompilation pipeline that
ties them together, and the Burnout 3 bring-up that proved the approach. Released under MIT.

### NoRain211 — [@NoRain211](https://github.com/NoRain211)
A run of correctness fixes across the disassembler, lifter and translator, found by
stress-testing the pipeline against a real title. Nearly all of it is the dangerous kind of bug:
the generated C compiles, links and runs, and is quietly wrong.

- **Control flow:** conditional tail calls that skipped the frame bridge, fall-through off the end
  of a function, and indirect calls that read their target after the return address was pushed.
- **Flags:** `repe cmpsb` and `repne scasb` result flags, `NEG` carry lost before `SBB`/`ADC`,
  `xor reg, reg` leaving carry set, and signed compares evaluated at the wrong width.
- **x87, SSE and MMX:** packed SSE lifted as a scalar, 904 x87 instructions lifted to comments,
  `FNSTCW`/`FNSTSW`, XMM state shared between blocks, fifteen MMX forms that lost the comparison
  before them, `PADDUSW`/`PSUBUSW`, and float-to-MMX conversion that ignored the rounding mode.
- **Instructions and dispatch:** `XLAT`, `BSF`/`BSR`, and manual overrides that direct calls
  bypassed.
- **Renderer and audio:** the depth/stencil state cache hash, A8 textures sampling black,
  colour blend factors copied into the alpha fields, `D3DFVF_XYZRHW` discarding RHW, the FVF
  position field, DirectSound cursors, and XAudio2 errors reported as success.
- **[doaxbv-re](https://github.com/NoRain211/doaxbv-re),** NoRain211's own project, is the source
  of the DirectSound model and ADPCM decoder adapted in `src/hle/`.

### DarthSidious666 — [@DarthSidious666](https://github.com/DarthSidious666)
- **`tools/abi_analysis`** — the missing stage that recovers calling convention, parameter count
  and return type, so generated signatures are real instead of cdecl with no parameters.
- **D3D8 texture translation** — all 66 Xbox formats mapped, cube and volume textures with
  per-face and 3D unswizzle, and the smoke test that made a change that size reviewable.
- **Kernel ordinal routing** — all 371 ordinals routed, the guest-VA to host-handle shadow table
  for events, semaphores and mutants, and allocator bridges that answer from the guest heap.
- **Two generator bugs that stop the build**, found with Black and Nightfire: `cmovcc` reading an
  undeclared carry, and recovered function names that collide with C or Win32 identifiers.
- **Toolchain portability** — the Ghidra pin, and taking one title's strings out of the tooling.

### dplewis — [@dplewis](https://github.com/dplewis)
- **`ReleaseMutex` reported success for a release it did not perform**, so the guest believed a
  held mutex was free.
- **The first macOS and POSIX work** — the Darwin half of `win32_compat`, honest `TODO`s where the
  platform had no equivalent, `xbox_wcslen` for 16-bit `WCHAR`, and the repair of a POSIX build
  broken four ways, including a race in the first use of SRW locks. The macOS port here started
  from the list of blockers they scoped.

### Tiptup300 — [@Tiptup300](https://github.com/Tiptup300)
Found that every documented step of the getting-started guide was broken, and did it on Linux,
where none of it had been tried (#1). That one report is the origin of the pipeline fix, and of
the repository having a licence file at all.

### SpringierTrain — [@SpringierTrain](https://github.com/SpringierTrain)
Asked whether Half-Life 2 could be ported (#12). The asking started it, and the carry-flag,
`bt`/`bts`, `rep movs`, atomics, per-thread TIB, function-boundary and SSE-compare fixes that came
out of making that title load a level all live in the shared toolkit.

### M0RSM4LLEO — [@M0RSM4LLEO](https://github.com/M0RSM4LLEO)
Reproduced and pinned down the getting-started failures (#2) with exact commands, tracebacks and
versions, including that `tools/xbe_parser` was the only tool without a `__main__.py`, and kept
testing through each fix, which is how the crash at the end of a full run was found.

Other names appear in the upstream history. Where a contribution is not described here it is
still recorded in `git log`, and we would rather list it properly: open a PR.

---

## Third-party code

Code adapted from other projects keeps its own licence and credit, listed in [NOTICE](NOTICE)
and summarised in the [README](README.md#license). That includes the MCPX APU and NV2A register
sources from [xemu](https://github.com/xemu-project/xemu).

---

## A note on AI-assisted contributions

Parts of this project, and some contributions to it, were developed with AI coding tools. That is
welcome here. What matters is that every change is understood, reviewed and verified by a human
before it lands. If you used an assistant, say so in your PR and make sure you can stand behind
the result.
