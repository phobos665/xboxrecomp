# Fifth title: Panzer Dragoon Orta, XDK 4928 — cold-start note

Started 19 September 2026 as a reconnaissance run alongside Outrun 2. It got
as far as **a lift that succeeds and a build that does not**. Nothing has run
yet. This is the note to pick it up from.

## What it is

| | |
| --- | --- |
| Title | Panzer Dragoon: ORTA |
| Title ID | 0x53450007 |
| XDK build | **4928** — a fifth distinct version (5849, 5344, 4721, 5028 are the others) |
| Graphics | D3D8, not LTCG |
| Engine | not RenderWare |
| `.text` | 2668 KB — the largest of the five |
| Image | 5156 KB |
| Kernel imports | 115 |
| Sections | 16, one demand-loaded (`$$XTIMAGE`) |
| Survey score | 15.92 (lower is better) |
| Concern flagged | a WMADEC section, and WMA decoding is not implemented |

Libraries are CalcSig, D3D8, D3DX8, DSOUND, LIBCMT, LIBCPMT, XAPILIB,
XBOXKRNL and XGRAPHC, all 1.0.4928. `LIBCPMT` is the C++ standard library, so
expect the same `std::map` shapes Outrun 2 is currently stuck in.

## How far it got

The XDK symbol step and all four pipeline stages ran clean:

```
py -3 -m tools.xdk_symbols "games/Panzer Dragoon Orta/default.xbe"
py -3 scripts/recompile.py "games/Panzer Dragoon Orta/default.xbe" \
    --work-dir games/_pipeline/pdorta/out --project titles/pdorta
```

| | |
| --- | --- |
| Functions found | 16,901 |
| Translated | 16,881 |
| Failed | **0** |
| Lines of C | 1,671,809 in 20 files |
| Unresolved call stubs | 81 |

CMake configured. The compile then failed, on two lines, in one function.

## The one thing blocking the build

```
recomp_0009.c(68378): error C2065: 'dr0': undeclared identifier
recomp_0009.c(68385): error C2065: 'dr0': undeclared identifier
```

Both are `eax = dr0;` inside `sub_0021118E`. `dr0` is an x86 debug register.
Nothing declares it, so the file will not compile, and one bad function stops
the whole title.

**It is two bugs, and they are both general.**

1. **The translator emits an undeclared identifier instead of a stub.** Every
   other instruction it cannot model degrades gracefully; this one produces C
   that does not build. Whatever it does for `iretd` and `lcall` — which this
   title also hits, and which did not stop the build — it should do for the
   debug and control registers. That is the fix that unblocks the title, and
   it is small.

2. **`sub_0021118E` is almost certainly not a function.** Read its body and
   it is obvious nonsense: `mov cs, [esi]`, `adc` against offsets like
   `0x39002110` and `-771743473`, `and [eax], eax` repeated. That is the
   disassembler walking data as if it were code. Fixing (1) makes the title
   build regardless, but this belongs on the function-discovery pile, because
   a misidentified region that happens to decode to something harmless is a
   silent version of the same problem.

## Next

1. Make the translator stub unknown register reads (`dr0`-`dr7`, `cr0`-`cr4`)
   the way it stubs other unimplemented instructions, then rebuild.
2. Run it and classify where it stops, against `CLAUDE.md`'s symptom table.
3. Expect the 4928 XDK to need its own signature work, as 4721 did. It is a
   fifth version, so whatever breaks is a fresh data point about what the
   project still treats as universal.
4. WMADEC is present. If audio start-up stalls somewhere no other title
   reached, that is the first place to look.
