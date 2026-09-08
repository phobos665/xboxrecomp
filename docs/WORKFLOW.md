# Bringing up a title, end to end

`INSTRUCTIONS.md` is the quickstart: run these commands, get generated C. This
is the campaign plan for the part that comes after — getting from "it compiled"
to "it is playable", without burning weeks on the wrong thing.

## Set expectations first

**Playable is a long way from booting, and booting is a long way from
compiling.** One title has reached playable with this toolkit. The most
advanced downstream port — with a purpose-built runtime and months of work —
reports the game booting at ~29 fps with save, audio and movie gates passing,
and still describes itself as not playable.

So plan for a campaign, not an afternoon. The good news is that the work is
extremely gated: you always know what the next blocker is, because the program
tells you. The failure mode is not "stuck with no information", it is "many
small things in sequence".

Two rules make the difference between progress and thrash:

1. **Never hand-edit generated C.** Fix the lifter and regenerate. A hand-edit
   is lost on the next pass and hides a bug that will hit the next title too.
2. **Track a frontier, not a feeling.** The frontier is the furthest *natural
   game event* you have observed — "reaches the title screen", not "runs for
   longer". A crash-free run that reaches nothing new is not progress.

---

## Phase 0 — Prove the toolchain before blaming the game

Do this once, before any game is involved. When something breaks later you want
to already know the lifter is sound.

```bash
python3 -m unittest discover -s tools -p "test_*.py" -t .
```

If you have a 32-bit MSVC available, also run the differential conformance
harness. It assembles snippets, runs them **on your real CPU**, lifts the same
bytes, runs the lifted C, and compares:

```bash
py -3 -m tools.conformance
```

This is a stronger oracle than it sounds. We target x86 and run on x86, so the
host CPU is the reference implementation — it cannot be wrong about what an
instruction does. Any disagreement is a lifter bug, found without a game.

**Gate:** tests green. If they are not, stop here.

---

## Phase 1 — Choose and extract

```bash
python3 scripts/survey_xbe_library.py /path/to/isos --xdk 5849
python3 -m tools.xiso unpack "Your Game.iso" -o game_files/
```

Prefer a title on an XDK you have already brought up, with a small `.text`, a
documented engine, and an empty `CONCERNS` column. See `INSTRUCTIONS.md` step 1.

**Gate:** you have `game_files/default.xbe` and you know its XDK build and
whether it is LTCG.

---

## Phase 2 — First generation and build

```bash
python3 scripts/recompile.py game_files/default.xbe
```

Then set up the project from `templates/new-game` and build it
(`INSTRUCTIONS.md` steps 4–5).

Before you run it, get a per-title read on how much kernel work is ahead:

```bash
python3 -m tools.kernel_audit.coverage game_files/default_analysis.json --list
```

That splits what is missing into "an implementation exists, it needs a bridge",
"this is a data export, it needs a value", and "nothing exists yet" — three very
different amounts of work. It also tells you early whether this title is going
to be cheap or expensive, while switching targets is still easy.

**Gate:** it links and runs, even if it does nothing.

---

## Phase 3 — The boot loop

This is where most of the time goes. One loop, repeated:

> run → read stderr → classify → fix → **regenerate** → run

```bash
./build/my_game 2> run.log
```

### The single most common first result

The program starts, makes two kernel calls, and exits cleanly. That reads like
success and is actually zero progress.

The usual cause is that the game's thread start routine was never discovered.
The address handed to `PsCreateSystemThreadEx` is only ever *pushed as an
argument* — nothing calls it — so static analysis never sees a function there,
it gets no dispatch entry, and the game thread silently never starts.

Feed the run back into discovery:

```bash
python3 -m tools.seed_from_log run.log game_files/default.xbe \
    --functions tools/disasm/output/functions.json \
    --seeds     config/seed_functions.json

python3 -m tools.disasm game_files/default.xbe --text-only \
    --seed-functions config/seed_functions.json
python3 scripts/recompile.py game_files/default.xbe --from identify
```

Add `--dry-run` first to see what it would seed.

**Do this every iteration.** Each run discovers indirect-call targets that
static analysis cannot see, and each regeneration turns them into real
functions. This loop is the main engine of early progress — not hand-written
overrides.

**Seeding is not free.** An address the game mentioned is not necessarily a
function — a garbage slot points at data just as easily, and seeding data splits
real functions and breaks the build far worse than the missing target did. The
tool gates every candidate on being in an executable section *and* decoding as a
function body, and it still warns: on one title, seeding two bad entries took a
boot from 34 assets loaded down to 1. If the frontier goes *backwards* after a
seed pass, suspect the seeds first.

### Reading the log

`INSTRUCTIONS.md` step 6 has the symptom table. The ordering that matters:

| Frontier | What you are fixing |
|---|---|
| Exits immediately | Thread start routine not discovered — seed and regenerate |
| CRT startup | Heap init, TLS, entry-point and ESP setup |
| Hardware init | Kernel ordinals, MMIO hooks, EEPROM/SMC values |
| Asset loading | Path translation, file I/O, disc layout |
| First frame | D3D8 device creation, texture formats, vertex declarations |
| Menus | Input, fonts, more texture formats |
| Gameplay | Everything else, plus timing and audio |

**Get to a black screen with no crashes before caring about rendering.** A
render bug you fix before the game is initialising properly is usually a
symptom of the init bug, and you will fix it twice.

### When you stop making progress

The discipline that ends the longest debugging sessions:

**If two runs stop at the same event, stop varying the run.** Do not try another
tweak. Instead: compare both logs, find the *earliest* point where they differ
from each other or from expectation, state one falsifiable mechanism, and test
the smallest change that would prove it wrong.

Keep each iteration bounded — if you cannot observe the result in about a
minute, you are not iterating, you are waiting.

### Overrides are the last resort, not the first

An unresolved `[ICALL]` is usually a **discovery** gap. Seeding fixes it for
every title; an override fixes it for exactly one and does not transfer. Reach
for `recomp_manual.c` only after seeding has failed.

When you do add one, **write down why** — what the toolkit gets wrong, and what
would let the override be deleted. Nothing enforces this, which is exactly why
it gets skipped, and an override whose rationale was never recorded can never be
re-evaluated. Keep them in `recomp_manual.c` and nowhere else:
`tools/recomp/manual_scan.py` reads that file to decide what *not* to generate.

**Gate:** the game reaches a menu you can navigate.

---

## Phase 4 — Correctness, not just liveness

Now the dangerous class appears: things that do not crash and are simply wrong.
Physics that drifts, RNG that diverges, timers that run at the wrong rate.

Bring up xemu as a reference oracle (`docs/technical/xemu-debugging.md`). Break
at the same function in both, dump state, find the first disagreement. Add a
state-dump hook to your build early so this is a flag rather than a rebuild.

Suspect **x87 precision divergence first** whenever behaviour differs from xemu
without any fault.

**Gate:** a full activity — one race, one match, one level — completes with the
same outcome as xemu.

---

## Phase 5 — Playable

"Playable" is worth defining concretely before you claim it, or you will keep
moving the line. A reasonable bar:

- [ ] Boots to the title screen unattended, every time
- [ ] Every menu is navigable and every option applies
- [ ] At least one full activity completes start to finish
- [ ] Saves persist, and reload correctly after a restart
- [ ] Audio plays: music, effects, and any voice
- [ ] Controller input is complete, including analog and rumble
- [ ] Frame pacing is stable enough to play, not just to observe
- [ ] It survives a long session without leaking or drifting
- [ ] No override in `recomp_manual.c` is a lie about game logic

That last one matters. Stubbing a wait to get past it is fine as scaffolding and
fatal as a finish line — it means the game is not doing what the game does.
Audit the override list before declaring playable.

---

## Phase 6 — Only now, enhancement

Widescreen, high resolution, higher frame rates, texture replacement,
modding — all of it is easier and safer once behaviour is correct, because you
have something to regress against. Enhancements built on top of an
unstable base make it impossible to tell an enhancement bug from a port bug.

The one thing worth doing *early* is the state-dump hook from Phase 4. It costs
little and it is what makes every later "did I break something?" answerable.

---

## What to send upstream

Anything you fix in the lifter, the kernel layer, or the D3D layer helps every
title and belongs upstream, not in a fork. Per-title findings — addresses,
overrides, layouts — stay local. The split is simple: if a fix mentions a
specific game, it is yours; if it mentions an instruction, an ordinal, or an
API, it is everyone's.
