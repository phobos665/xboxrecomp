# xboxrecomp fork — working context

Fork of `sp00nznet/xboxrecomp`. Goal: static recompilation of original Xbox titles into
native executables, generalised beyond the single proven target.

This file lives at the repo root; Claude Code reads it automatically each session.

---

## The framing to hold onto

**The performance win comes from HLE at the D3D8 boundary — not from lifting CPU code, and
not from the graphics backend.**

Guest is x86-32 little-endian, host is x86-64 little-endian. No byte swapping, 8 guest GPRs
into 16 host GPRs, guest EFLAGS semantics *are* host EFLAGS semantics. Same-ISA translation is
already near-native, and a 733 MHz Coppermine P3 leaves enormous headroom. Frame time goes into
NV2A work — push-buffer parsing, PGRAPH state decode, texture cache invalidation, write-watch
traps — none of which static CPU recompilation touches.

Consequences:

- Recompiled output will **not** beat xemu on a desktop x86 machine unless D3D8 is HLE'd.
- Naive lifted C can be *slower* than a tuned JIT. There is no deficit to start from, so
  codegen quality has to be good just to break even.
- The genuine wins are: HLE'ing D3D8, ARM hosts (where x86-32 → ARM64 is no longer same-ISA
  and lifting genuinely pays), moddability, and portability once the memory model is fixed.

**Why HLE generalises here but not on the 360:** there is no `d3d8.dll` on the Xbox — D3D8 is
statically linked into the XBE, and it is a known, versioned library. Cxbx-Reloaded has spent
fifteen years building OOVPA signature databases identifying it *per-XDK-build*. So the HLE
boundary is per-XDK-version, not per-game, and there is a manageable number of those.

---

## Corrections to the upstream README

Verify against the repo rather than trusting the README. Known discrepancies as of August 2026:

| README says | Reality (per `docs/technical/gap-analysis.md`) |
|---|---|
| NV2A push-buffer interception is a core feature | Push-buffer parsing is a **stub**, marked "N/A — D3D8 API intercept instead". The project already does D3D8 HLE. |
| "115 of 366 ordinals resolved, 55 bridged" | Do not trust any number written down; the useful question is per-title, not global. Run `py -3 -m tools.kernel_audit.coverage <analysis.json> --list`, which splits what is missing into "needs a bridge wrapper", "is a data export", and "does not exist yet". Note an ordinal with an `xbox_*` implementation but no bridge silently returns 0. |
| Portable C output targeting ARM, RISC-V, WASM | Memory model uses `CreateFileMapping` + fixed-address `MapViewOfFileEx` at guest VAs. Win32-only in practice. |
| Burnout 3 is the proven target | True, and it is a **D3D8LTCG** build on XDK 5849 — so LTCG is not disqualifying. |

The texture layer is the real generalisation blocker: 17 of 66 formats, mipmap level 0 only,
no palette lookup for P8, no texture coordinate generation.

---

## Planned changes, in dependency order

The first four get baked into generated code and are expensive to retrofit. **Do these before
any bulk codegen.**

### 1. Memory model
Move from fixed-VA mapping to base+offset. Reserve 4 GB. Keep guest pointers **32-bit** — they
live inside guest structs, so widening them breaks every layout. Implement mirror regions as
multiple mappings of one shared object. This is what makes the project portable off Win32.

### 2. Register model
Replace global `g_eax`-style registers plus the simulated stack in a guest memory array with a
per-function context struct and local temporaries. The globals defeat aliasing analysis and
register allocation, which is why naive lifted output loses to a JIT.

### 3. x87 policy
The Xbox is SSE1-only, so era compilers emit x87 for doubles. `long double` is 80-bit on x86
GCC/Clang, 64-bit on MSVC, 128-bit on AArch64 — bit-exactness is not portable for free.
Decision: `double` by default, with a per-function soft-float80 escape hatch. Settle this
before the lifter emits its first FP instruction.

### 4. Threading
The Xbox is uniprocessor. Guest code raises IRQL as mutual exclusion and spins without
barriers. Keep the cooperative single-thread model, but inject **yield checks at loop
back-edges** so a spinning guest thread cannot deadlock.

### 5. Separate engine from per-title data — **upstream owns this now**
Overrides stay in `recomp_manual.c`. Upstream's `tools/recomp/manual_scan.py` treats that
file as the single source of truth for which `sub_XXXXXXXX` are hand-defined, and the
recompiler reads it to decide what *not* to generate — so a second override file causes
duplicate symbols and unresolved externals. A fork-local split was tried here and reverted.

The mandatory-reason-per-override idea is still right and still unenforced. The place to
land it is upstream, as a convention plus a check over `recomp_manual.c`, not as a
competing file.

The kernel thunk table is already per-title upstream: `xbox_kernel_init()` reads ordinals
from the mapped XBE, keeps `g_thunk_ordinals` only as a no-XBE fallback, and adds
`xbox_kernel_set_ordinal_remap()` on top.

### 6. Then, in rough order of what they unlock
- **Texture layer** — table-drive formats, mip levels, P8 palette, texcoord generation.
  XDK-independent, and the thing most likely to stop title #2 even on the same XDK.
- **Function discovery quality** — better indirect-branch analysis, vtable recovery, seeding.
  Every unresolved ICALL becomes a per-game override that doesn't transfer, so this directly
  attacks per-title cost.
- **XDK signature coverage** — an *identification* problem, not a reimplementation one. The
  D3D8 API surface is stable; what changes per build is where functions sit. Much of this is
  porting Cxbx-Reloaded's OOVPA work. LTCG builds are harder but demonstrably tractable.
- **Kernel ordinal coverage** — driven by what titles call, not by XDK version; accumulates
  naturally. Audit per-title with `tools.kernel_audit.coverage`.
- **LLE fallback** — for titles that hand-roll push buffers or defeat signature matching.
  Shipping working-but-slow, then converting to the fast path, is what makes this a toolkit
  rather than a collection of per-game hacks.
- ~~**Pipeline driver script**~~ — done: `scripts/recompile.py`, defaults to `--game-only`,
  with `--from`/`--only` to resume part-way.

---

## Pipeline

Tools run as Python modules from the repo root (`py -3` on Windows, `python3` on Linux).

```
extract-xiso  ->  tools.xbe_parser  ->  tools.disasm  ->  tools.func_id
                                                              |
                     cmake build  <-  tools.recomp  <---------+
```

All four stages at once (this is the normal path):

```bash
python3 scripts/recompile.py game_files/default.xbe
```

Or stage by stage:

```bash
py -3 -m tools.xbe_parser  game_files/default.xbe --json game_files/g_analysis.json
py -3 -m tools.disasm      game_files/default.xbe --text-only -v
py -3 -m tools.func_id     game_files/default.xbe -v
py -3 -m tools.recomp      game_files/default.xbe --game-only --split 1000
```

The `--json` from step one is **required** by the disassembler — it reads section layout from
it. Keep it beside the XBE.

Use `--game-only` when bringing up a new title. Lifting CRT and XDK code you intend to HLE away
wastes compile time and debugging attention. Switch to `--all` only when needed.

---

## Debug loop

Build with `--game-only` → run → read stderr → classify → fix → rebuild. Most project time
lives here.

| stderr shows | Cause |
|---|---|
| `[ICALL] unknown target 0x... from RVA 0x...` | Target never detected as a function. Re-run discovery with `--seed-functions`, or add a manual override. |
| Access violation at `0xFD......` | GPU MMIO — NV2A hooks not initialised |
| Access violation at `0xFE......` | APU MMIO — audio hooks not initialised |
| `SKIP-READ` | Unmapped access; often a native pointer where a guest VA was expected |
| Infinite loop | Waiting on hardware state — stub the wait or fake the state |
| Stack overflow | Wrong ESP at entry, or runaway recursion |
| **Subtly wrong physics or RNG, no crash** | **x87 precision divergence — suspect this first when behaviour differs from xemu without a fault** |

Overrides live in `recomp_manual.c` and are checked before the auto-generated table, so they
always win. `manual_scan.py` parses that file to decide what not to generate, so keep them
there and nowhere else. **Log every override with its reason at the moment you add it** —
nothing enforces this yet, which is why it gets skipped.

**Bring-up order:** CRT startup → hardware init → asset loading → menus → gameplay. Get to a
black screen with no crashes before caring about rendering.

**Reference oracle:** run the title in xemu with its GDB stub. For the no-crash divergence case,
bisect — break at a function in xemu, dump registers, dump the same context struct at the same
point in the lifted build, find the first disagreement. Add a state-dump hook to lifted output
early so this is a flag rather than a rebuild.

Ghidra is static analysis of the XBE (a second opinion on `tools.func_id`), not a live view of
a running game. Its debugger module can attach to xemu's GDB stub, but expect to fight it over
i386 detection. Not a prerequisite.

---

## Choosing the next target

**LTCG status and engine cannot be determined from wikis** — LTCG is a per-build linker setting
recorded nowhere public. Run the survey script over extracted discs (it exists now, and ranks
candidates; `scripts/make_test_xbe.py` builds synthetic XBEs for testing it without game files):

```bash
python3 scripts/survey_xbe_library.py /path/to/extracted/discs --csv survey.csv
python3 scripts/survey_xbe_library.py /path/to/one/default.xbe --verbose
```

It reads XBE headers directly and reports XDK version, D3D8 vs D3D8LTCG, library list,
RenderWare detection and version, `.text` size, kernel import count, and demand-loaded
sections. Treat the ranking as a starting point — it cannot see hand-rolled push buffers,
threading complexity, or whether you want to play the game.

Good first targets: small `.text`, RenderWare or another documented engine, no XONLINE, no
WMADEC, few demand-loaded sections. Criterion titles (Burnout 1/2) overlap most with the
existing reference implementation. Avoid GTA as a *first* target despite being canonical
RenderWare — large, threaded, streaming-heavy. Avoid 2001–2002 launch titles, which sometimes
use debug-era XDK patterns.

**Strategy:** bring up a second title on XDK 5849 first. Anything that breaks is by definition
something the project wrongly treated as universal. A third title on a different XDK then
isolates what is version-specific. That converts "it only does Burnout 3" into a work queue.

---

## Legal

The toolkit ships engine and tooling code, not game content. Recompiling requires game files
from a copy you own. Do not distribute recompiled binaries containing game code or assets, and
do not redistribute XDK library code recovered during signature work.

---

## Note on xboxlle-probe

`mstan/xboxlle-probe` is an nxdk agent listening on TCP 4380 that answers requests: named
read-only CPU/NV2A probes, guarded RAM/MMIO/flash reads, and — in armed unsafe mode — writes
and uploaded x86 payload execution. It is a **poke-and-read hardware oracle, not an instruction
tracer**, and has no Ghidra integration. Its named probes use a fixed register set tested on
one Xbox v1.1, and `controller-s-hub` is an admitted stub.

Its real value here is as ground truth for x87 precision — upload small test payloads via
`EXEC`, read results back — and for MMIO read side effects on the LLE fallback path. Flash
dumps contain per-console secrets and unique identifiers; never commit them.
