# Third title: Jet Set Radio Future, XDK 4134

Started 20 September 2026. **Not running yet**: it boots, creates its D3D
device, and then hangs in its own allocator. Two things that were wrong were
wrong in the toolkit rather than in this title, which is the point of a third
XDK.

---

## What it is

| | |
| --- | --- |
| Title ID | `0x5345000A` |
| XDK build | **4134** — older than TimeSplitters 2's 4721 and Burnout's 5849 |
| Direct3D | D3D8, not LTCG |
| Engine | not RenderWare |
| `.text` | 1518.8 KB |
| Kernel imports | 120 |
| Libraries | D3D8, DSOUND, LIBCMT, LIBCPMT, XAPILIB, XBOXKRNL, XGRAPHC |

`tools.xdk_symbols` finds 363 symbols, 131 of them D3D8 functions, so the OOVPA
database covers 4134 well. The pipeline lifts 9136 functions with none failed
and the project builds clean first time.

```bash
py -3 -m tools.xdk_symbols "games/Jet Set Radio Future/default.xbe"
py -3 scripts/recompile.py "games/Jet Set Radio Future/default.xbe" \
    --work-dir games/_pipeline/jsrf/out --project titles/jsrf \
    --seeds config/seeds/5345000A.json
```

---

## What it proved was wrongly universal

### 1. The GPU time fence matcher read one register layout

`mirror_gpu_time_fence()` in `src/hle/hle_d3d8.c` took the two device offsets
from `D3D_BlockOnTime`'s prologue, matching literal bytes:

```
56                push esi
8B 35 <g_pDevice> mov  esi, [D3D_g_pDevice]
8B 46 <A>         mov  eax, [esi + A]
```

4134 keeps the `time` argument in esi and puts the device in **edi**:

```
0x00191441  8B 74 24 08     mov esi, [esp+8]        ; the argument
0x00191446  8B 3D E0DC1900  mov edi, [0x19DCE0]     ; the device
0x0019144C  8B 47 34        mov eax, [edi + 0x34]
0x00191451  8B 47 30        mov eax, [edi + 0x30]
```

so every `8B 46` is `8B 47` and the match failed, leaving the title waiting on
a fence nothing advances — the exact hang that code exists to prevent.

It now scans the prologue for the load of `D3D_g_pDevice` wherever it sits,
takes the destination register from the ModRM byte, and reads the offsets
against that register. Matching on the address also rejects a prologue that
reads some other global, which used to need a separate check.

**The offsets themselves were the same as 4721** (`+0x30` submitted, `+0x34`
pointer to completed). Only the register differed — which is why a table keyed
by XDK version would have been the wrong shape, and reading the prologue is
right.

### 2. A function static analysis never found

`0x00154DAA`, called from `0x00154E1F`, was skipped as `[STUB]`. The chain from
there was worth recording because none of it pointed at the real cause:

1. the skipped call left a failure code (`0x80004005`, E_FAIL) on the stack;
2. its caller raised a C++ exception (`0xE06D7363`);
3. this runtime cannot unwind, so the throw returned and the unwind ran anyway;
4. the heap came out of it with a free-list node linked to itself;
5. the allocator hung walking that list.

Seeded in `config/seeds/5345000A.json` with that chain as its note. The
`[STUB]`, the exception and the E_FAIL are all gone.

---

## Where it stops

The main thread spins at 96% in `sub_001497DC`, the title's allocator, walking
free-list bucket 0. `RECOMP_SAMPLE=500` names it; the loop is a list walk that
ends when it reaches the bucket head:

```c
loc_00149B28:  if (ecx == eax) goto done;        /* eax back at the head */
               if (LO16(edx) <= MEM16(eax - 8)) goto done;  /* big enough */
               eax = MEM32(eax); goto loc_00149B28;
```

The list contains one node, `0x0105EE68`, whose `next` is itself, so neither
exit is ever taken.

**It is a double free, and the node is not corrupt.** Watching the address
through `RECOMP_WATCH_WRITE` gives the sequence: zeroed, then linked correctly
(`-> 0x00F81180`, the bucket head), written correctly twice more, then
overwritten with its own address. The insert that does it is a sorted insert
walking bucket 0 (`0x0014A074`–`0x0014A0A0`), and the walk finds the node
already in the list before finding its place, so `node->next = <walk position>`
writes the node into itself.

The guest call chain at that moment, caught by faulting deliberately when the
walk position equals the node being inserted:

```
sub_0015FCC0 -> sub_0015FC40 -> sub_0015FD40 -> sub_00160EA0
             -> sub_00160C30 -> sub_0017C965 -> sub_0014A821 -> free internals
```

### What has been ruled out

- **Lifter miscompilation.** No `movsx: unhandled` markers and no bare narrow
  reads in 1,047,085 lines of generated code.
- **The SEH frame.** `sub_0017D1F8` is `__SEH_prolog` and ends `lea ebp,
  [esp+0x10]`, which is exactly what `detect_seh_helpers` matches, so the frame
  the allocator's locals hang off is right.
- **`SET_LO16` on a pointer.** `edi = 0x0105021A` at the bad write looks like a
  size stuffed into a pointer's low half, but `66 8B 7D DC` really is
  `mov di, word [ebp-0x24]` and only `di` is read.
- **Uncommitted memory.** The node sits past the first commits, but with a
  large `--kernel-log` the title is seen committing up to `0x01061000`, which
  covers it. The earlier doubt came from the 200-call log budget truncating.
- **Exceptions.** Zero `[EXCEPTION]`, `[STUB]`, `[THROW]` and `[INT3]` since
  the seed. `_except_handler3` keeps appearing in `callers:` lines, but those
  are a heuristic stack scan picking up stale frames.

### Two traps worth knowing before picking this up

- **`sub_00149F5E+0x946` is a misattributed symbol.** Two rounds of
  instrumentation went into that function's insert sites and neither fired,
  because the linker folds identical functions. `docs/technical/memory-watchpoints.md`
  says to trust the `callers:` line over the symbol; it is right.
- **The registers in the watchpoint dump are the answer.** `ecx == edx ==
  0x0105EE68` with `eax = node - 8` and `esi = 0x00F81180` identified the store
  and the code path in one go, after the symbol had sent two attempts the wrong
  way.

### Found: a LOCK prefix cost the refcount its branch

Instrumenting `free`'s entry gave both call sites, and from there the answer
was one line of generated code. The objects are reference counted through the
COM idiom:

```
lock xadd [this+8], edx   ; edx = -1, so refcount--
jne  still_referenced     ; sum non-zero: somebody else still holds it
push 1
call [vtable+0x48]        ; deleting destructor, flags=1: destruct AND delete
```

which lifted to `if (_flags) goto still_referenced;` — and `_flags` is declared
`int _flags = 0` in every generated function and assigned nowhere. The branch
could never be taken, so **every `Release()` destroyed the object however many
references remained**, and the next `Release()` freed it a second time.

The cause was `"lock xadd"` sitting in the lifter's `_FLAGS_UNDEFINED`,
described as "complex flag behavior". LOCK changes atomicity, not arithmetic:
a locked instruction leaves exactly the flags its unlocked form leaves. The
prefix is now stripped where flags are tracked. `"lock cmpxchg"` had the
quieter half of the same bug — it matched no list at all, so the *previous*
instruction's flags were left standing as if they were its own.

**It was never JSRF-specific.** Counting locked atomics across the titles
lifted here: 87 in JSRF, 56 in Panzer Dragoon Orta, 50 in Marvel vs Capcom 2,
5 each in Black and Burnout 2, and **none at all in TimeSplitters 2 or Outrun
2** — which is why TimeSplitters 2 was unaffected and could never have
revealed this.

### Where it stops now

Past the hang, and a long way past it. The title runs through XAPI start-up,
loads its input bindings, opens a pad, starts the APU, finds both DSP
doorbells and has DirectSound playing four buffers at 48 kHz. It then makes
two indirect calls through pointers that are not code —

```
[ICALL] target 0x01054A70 is not code (call #1297)
[ICALL] target 0x00000000 is not code (call #1298)
```

— and writes a launch data page naming its own title ID with an empty path,
which `HalReturnToFirmware` routine 2 turns into an exit. The header layout is
confirmed against Cxbx-Reloaded's `LAUNCH_DATA_HEADER`
(`dwLaunchDataType`, `dwTitleId`, `szLaunchPath[520]`), so the fields are being
read correctly; `type=1` is not one of its four documented constants.

Those two wild pointers are the next thing. The first is a heap address, which
means a vtable slot or callback holding data rather than a function — the same
shape as an object used after it was destroyed, so it is worth checking
whether any premature destruction survives the fix before looking further.
