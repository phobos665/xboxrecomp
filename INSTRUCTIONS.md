# Recompiling a game

A start-to-finish walkthrough. Eight steps, in order.

This turns an Xbox game you own into a native executable. You need the game
files from your own disc — none ship here, and none ever will.

> **New to the project?** Read this page top to bottom once before running
> anything. Each step assumes the previous one worked.

---

## 0. Install the dependencies

**Linux / WSL:**

```bash
bash tools/linux/install_deps.sh
pip install capstone
```

**Windows:** Visual Studio 2022 with the C++ workload, CMake 3.20+, Python 3.10+,
then `pip install capstone`.

---

## 1. Pick a target

Some games are far easier than others, and the two things that matter most —
which XDK built the disc, and whether it used LTCG — are not on any wiki. They
are in the XBE header, so read them:

```bash
python3 scripts/survey_xbe_library.py /path/to/your/extracted/discs
```

You get a ranked table. Lower score is a better first target.

```
 SCORE  TITLE                     XDK D3D       RW        .text  IMP  CONCERNS
  -2.0  Small RW Racer           5849 D3D8      3.7.0.0    256K    5
  17.0  Big Online Shooter       5233 D3D8LTCG  -         1536K    5  XONLINE; WMADEC
```

**What to look for:** a small `.text`, a documented engine like RenderWare, and
an empty `CONCERNS` column. Anything listed there is a subsystem the toolkit
does not implement yet, and it will stop you before you see a frame.

**What to ignore:** `D3D8LTCG`. It makes signature matching harder but is not
disqualifying — the proven target is itself an LTCG build.

The ranking reads the header only. It cannot see hand-rolled push buffers or
threading complexity, so treat it as a shortlist, not a verdict.

> **Strongly recommended for your first title:** pick one built with the same
> XDK as a title already known to work. Anything that breaks is then, by
> definition, something the project wrongly treated as universal — which makes
> it a bug worth fixing rather than a mystery.

---

## 2. Extract the disc

Use [extract-xiso](https://github.com/XboxDev/extract-xiso):

```bash
extract-xiso -x "Your Game.iso" -d game_files/
```

You want `game_files/default.xbe` plus the game's data files beside it.

---

## 3. Run the pipeline

One command:

```bash
python3 scripts/recompile.py game_files/default.xbe
```

That runs four stages in order, each feeding the next:

| # | Stage | What it does |
|---|---|---|
| 1 | `parse` | Reads the XBE header, sections and imports |
| 2 | `disasm` | Disassembles `.text`, finds functions and cross-references |
| 3 | `identify` | Classifies each function: CRT, RenderWare, or game |
| 4 | `lift` | Translates the game's x86 into C |

It defaults to **game code only**, which is what you want. Lifting the CRT and
XDK code you intend to replace with native implementations costs compile time
and debugging attention for code you will never run. Use `--all` only when you
have a specific reason.

Generated sources land in `src/game/recomp/gen/`.

**If a stage fails**, fix the cause and resume where you left off rather than
starting over:

```bash
python3 scripts/recompile.py game_files/default.xbe --from identify
```

---

## 4. Set up your project

Copy the template and fill in three values:

```bash
cp -r templates/new-game ../my-game
```

Edit `../my-game/src/main.c`:

```c
#define YOUR_GAME_ENTRY_POINT   0x00011000              /* from step 3 */
#define YOUR_GAME_XBE_PATH      "game/default.xbe"
#define YOUR_GAME_DIR           "game"
```

The entry point was printed by stage 1. To see it again:

```bash
python3 -m tools.xbe_parser game_files/default.xbe | grep "Entry Point"
```

---

## 5. Build

```bash
cd ../my-game
cmake -B build -DXBOXRECOMP_DIR=../xboxrecomp
cmake --build build
```

---

## 6. Run, read stderr, fix, repeat

This is where nearly all of the work is. Run it, read the error, look it up
below, fix, rebuild.

```bash
./build/my_game 2> log.txt
```

**Aim for a black screen with no crashes before you care about rendering.**
Bring-up goes in this order, and skipping ahead wastes time:

> CRT startup → hardware init → asset loading → menus → gameplay

### What the errors mean

| stderr shows | What's wrong | What to do |
|---|---|---|
| `[ICALL] unknown target 0x...` | A call target was never detected as a function | Re-run discovery with `--seed-functions` |
| Access violation at `0xFD......` | GPU register access | NV2A hooks not initialised |
| Access violation at `0xFE......` | Audio register access | APU hooks not initialised |
| `SKIP-READ` | Unmapped memory access | Usually a native pointer where a guest address was expected |
| Hangs forever | Waiting on hardware state that never changes | Stub the wait, or fake the state |
| Stack overflow | Wrong ESP at entry, or runaway recursion | Check your entry point value |
| **Wrong physics or RNG, but no crash** | **x87 floating-point divergence** | **Suspect this first when behaviour differs from xemu without any error** |

That last row is the one that costs people days. If the game runs but behaves
subtly wrongly, stop looking for a crash — compare against xemu instead
(`docs/technical/xemu-debugging.md`).

### When you have to patch a function

Sometimes a function must be replaced by hand. Those go in **one file**:
`src/title_overrides.c`.

```c
static void stub_00067890(void) {
    g_eax = 0;
}

const recomp_override_t g_title_overrides[] = {
    RECOMP_OVERRIDE(0x00067890, stub_00067890,
        "Spins on an APU DMA bit the audio HLE never sets; "
        "remove once apu_vp reports buffer completion"),
    { 0, 0, 0, 0 }
};
```

The reason is **required** — the code will not compile without it, and will not
start if it is blank. This is deliberate. An override with no recorded reason
can never be re-evaluated, because nobody can tell whether the bug it works
around still exists. Write what the *toolkit* gets wrong and what would let the
override be deleted, not just the symptom.

Never edit `recomp_manual.c`. That is engine code shared by every title; if a
fix belongs there, it belongs in the toolkit for everyone.

---

## 7. The two tools worth adding

You can get a long way on stderr alone, but two external tools change what is
possible. Each answers a different question — reach for the right one.

### xemu — "what does the real thing do?"

xemu runs the same game correctly. Launch it with its GDB stub and you have a
reference to compare against:

```bash
xemu -s -S      # -s opens GDB on :1234, -S waits for you to attach
```

Use it for anything about **live** behaviour: what a kernel call actually
returns, what a struct contains at a given moment, what value a register holds
when a function is entered.

This is the only practical way to solve the no-crash divergence case. Break at
the same function in both, dump state, and find the first disagreement. Add a
state-dump hook to your build early so this is a flag rather than a rebuild.

Details in `docs/technical/xemu-debugging.md`.

### Ghidra — "where is this, statically?"

Ghidra answers questions about the binary at rest: function boundaries, cross
references, what calls what. It is a second opinion on `tools.func_id`, not a
view of a running game. Its debugger can attach to xemu's GDB stub, but expect
to fight it over i386 detection — not worth it as a first step.

The toolkit already ships an automated path that is usually enough:

```bash
bash tools/ghidra_naming/run_ghidra.sh
```

That runs Ghidra headless, recovers real CRT and XDK symbol names, and merges
them into `functions.json`, so generated C reads `RwMatrixMultiply` instead of
`sub_00123ABC`. See `tools/ghidra_naming/README.md`.

**If you drive Ghidra interactively (including via an MCP server), keep queries
bounded.** Ask for one specific thing — this function's bounds, this address's
xrefs — and record which binary you asked about. Open-ended exploration
produces answers you cannot reproduce later, and an address is meaningless
without knowing which build it came from.

### Which one

| Question | Tool |
|---|---|
| What does this kernel call really return? | xemu |
| What is in this struct at this moment? | xemu |
| Why does my build differ from the real game? | xemu |
| Where does this function start and end? | Ghidra |
| What calls this address? | Ghidra |
| What is this function's real name? | `ghidra_naming` |
| What does this XDK/NV2A register mean? | Public Cxbx-Reloaded or nxdk sources |

Read public sources for semantics, then implement independently.

---

## 8. Know when it isn't your bug

An unresolved `[ICALL]` is usually a gap in **function discovery**, not a quirk
of your game. Re-running discovery fixes it for every title; an override fixes
it for exactly one. Reach for the override only after discovery has failed you.

Likewise, an unimplemented kernel ordinal is a toolkit gap. To see where the
coverage currently stands:

```bash
python3 scripts/audit_kernel_ordinals.py --list-gaps
```

---

## Where things live

| Path | What |
|---|---|
| `scripts/` | Survey, pipeline driver, coverage audit |
| `tools/` | The four pipeline stages |
| `src/` | Runtime: kernel, D3D8, audio, input HLE |
| `templates/new-game/` | Starting point for a title |
| `docs/pipeline/` | Detailed stage-by-stage reference |
| `docs/technical/` | Memory layout, register model, xemu debugging |

## Further reading

- `docs/GETTING_STARTED.md` — a longer tour of the same ground
- `docs/technical/lessons-learned.md` — mistakes worth not repeating
- `docs/technical/xemu-debugging.md` — using xemu as a reference oracle

## Legal

This toolkit ships engine and tooling code, not game content. Recompiling
requires files from a copy you own. Do not distribute recompiled binaries
containing game code or assets, and do not redistribute XDK library code
recovered during signature work.
