# Lessons from a downstream port

[`doaxbv-re`](https://github.com/phobos665/doaxbv-re) is a native PC port of Dead
or Alive Xtreme Beach Volleyball that pins **this toolkit** as a submodule
(`tools/xboxrecomp`) and drives it against a real title. It is the closest thing
we have to a second user, and it has gone further than our own reference target:
its status notes report boot delivery at roughly 29.4–29.6 fps with save,
audio and movie playback gates passing locally.

That makes it worth reading carefully. What follows is what it teaches, and what
this repository has already changed as a result.

---

## 1. It found real lifter bugs, and there were more

The port carries `tools/xboxrecomp-patches/fsubp-destination.patch`, which fixes
`FSUBP` writing `ST(1)` regardless of its explicit destination operand.

Checking that against our fork showed the same bug class was far wider here,
because our lifter predates the upstream revision the patch applies to. `FADD`
and friends with a memory operand ignored the operand entirely *and* popped;
`FSUBR`, `FDIVR` and the `FI*` integer forms had no handler at all and lifted to
a bare comment; `FXCH` ignored its operand; `FISTP` never popped. See the x87
commit and `tools/recomp/test_lifter_fpu.py`.

**The lesson is not "x87 was broken".** It is that a second consumer running
real code finds correctness bugs that a single working title never will, because
the reference target simply does not happen to execute those encodings. Every
one of these produced wrong numbers with no crash and no diagnostic.

**What we do about it:** a downstream patch directory is a bug report. Read it
against our own tree rather than assuming it applies as written.

---

## 2. Their test strategy solves our CI problem

We cannot ship game code, so at first glance there is nothing to test. Their
answer is worth copying wholesale:

- **Synthetic instructions.** Build `Instruction`/`Operand` objects directly,
  run the lifter, assert on the emitted C. No game files, no compiler, no
  generated program. Their `check_fsubp.py` is 40 lines and catches a bug that
  silently corrupts physics. Ours is now `tools/recomp/test_lifter_fpu.py`.

- **A fixture at the dispatch seam.** `runtime_public_fixture.c` is a
  hand-written function that occupies the same seam a generated function would.
  It exercises guest register, memory and dispatch behaviour without containing
  any game-derived code, so the runtime is testable in public CI.

- **Negative tests on the custody gates.** Their CI does not just check that the
  authenticated build works; it proves configuration *fails closed* when given a
  generated source without its hash, or with a wrong one. A gate nobody has
  watched fail is not known to be a gate.

---

## 3. Models, adapters and presenters

The runtime is 146 files split on a strict rule: 36 `*_model.c`, 28
`*_adapter.c`, and a presenter, with 38 test files.

| Layer | Role | Testable? |
|---|---|---|
| `X_model.c` | Pure logic and state. No host API, no interception. | Yes, directly |
| `X_adapter.c` | The seam where guest D3D8/kernel calls arrive | Thinly |
| `X_presenter.c` | Host delivery (D3D11, XAudio2) | Only on a host |

Their rule, from `AGENTS.md`: *"Keep models separable from interception and host
delivery details."*

This is why they can unit-test rendering logic and we cannot. Our
`src/d3d/d3d8_device.c` is 1291 lines mixing all three concerns, so there is no
seam at which to assert anything. Worth restructuring behind this split as the
texture layer gets generalised.

---

## 4. Recompilation as scaffold, not destination

Their framing:

> The whole-program recomp is a temporary scaffold: first make the program run
> using native kernel, input, audio, and D3D8 replacements, then replace
> game-owned generated functions in coherent clusters.

Their loop is: pick a function that already runs, read its generated form
alongside bounded static analysis, write real C with real names and types,
register it so it wins over generated dispatch, and keep it only if behaviour
holds. Crucially, they work in **data or call clusters** — generated neighbours
still depend on fixed guest addresses and layouts, so a structure can only be
redesigned once every function that touches it has been replaced.

That is a different end state from "lift everything and ship it", and it is
compatible with our override mechanism: `title_overrides.c` is exactly the seam
their step 4 uses.

---

## 5. Tool routing

Their `AGENTS.md` gives the clearest statement of which tool answers which
question:

- **Translation or regeneration** → the pinned toolkit, output kept ignored.
- **Static addresses, xrefs, function bounds** → *one bounded* Ghidra query,
  recording the binary identity.
- **Real kernel or hardware behaviour** → xemu as a live oracle.
- **XDK or NV2A semantics** → public Cxbx-Reloaded, nxdk or public headers,
  then implement independently.

Two details matter. "Bounded" means ask a specific question rather than explore,
so the answer is reproducible. "Record the binary identity" means an address is
meaningless without knowing which build produced it — the same title on another
disc revision has different addresses.

This is now summarised in `INSTRUCTIONS.md` step 7.

---

## 6. Discipline worth stealing

- **Never hand-edit generated C.** Fix the lifter and regenerate. Their rule;
  it is why their patches exist as upstream patches rather than local edits.
- **General lifter fixes belong upstream, not in a game-specific fork.** They
  explicitly refuse to vendor a fork.
- **Forced state is a hypothesis, not a result.** Report what was observed.
- **When two runs stop at the same place, stop varying the run.** Compare both,
  find the earliest divergence, state one falsifiable mechanism, test the
  smallest safe change. This is the discipline that ends x87-divergence hunts.
- **A green test suite is regression evidence, not progress.** Their public
  status is blunt that CI success proves the tree builds, not that the game
  boots.

---

## 7. Custody

`public-export.json` is an allowlist-based export manifest: tracked paths, a
maximum file size, forbidden extensions (`.xbe`, `.iso`, `.xpr`, `.xmv`, `.sav`
and more), quarantined directories, and pinned submodule commits. Even synthetic
binary test fixtures are justified individually by SHA-256 with a written
reason.

Our own rule is the same in substance and looser in enforcement: engine and
tooling code only, never game content. If this repository ever grows a public
export, that manifest is the model.

---

## Not applicable to us

Their runtime deliberately does **not** use the experimental runtime code in
this repository: D3D8 is replaced at the API level and no NV2A emulator sits
beneath the game. Their tooling README says so explicitly.

That is a reasonable choice for a single-title port and it matches the
architectural framing in `CLAUDE.md` — the performance win is D3D8 HLE, not
emulating the GPU underneath. It does mean their runtime is not a drop-in source
of code for us, only of design.
