# Lifted code quality: a review of the register-model report

*1 Oct 2026. Reviews "Lifted code quality and the register-model rewrite" (OutRun 2, same date),
called "the report" below. Every claim was checked against this tree. The codegen claims were
reproduced with the real lifter, the GCC 13 and Clang 18 compilers and a timing harness
(`tools/codegen_bench`). That harness is new; it runs on Linux and needs no game files.*

**Summary.** The report's diagnosis is right. Lifted code is slow because every guest register is an
addressable thread-local that any guest store might alias, so it is reloaded after every store.
Its recommendation to cache registers in locals and keep the globals as the ABI is also the right
shape. Three of its supporting claims do not hold as written, and the plan misses two things
that decide whether the rewrite is a win at all:

- **Naive write-back at calls makes call-heavy code slower, not "a few % less" faster.** It
  stores all seven registers before every call and reloads all seven after. Measured on a
  one-call loop, that is 15-22% *slower* than today. Syncing only the registers that are dirty
  (before the call) or live (after it) makes the same loop 21-32% *faster*. The emitter needs
  liveness from day one.
- **Moving the x87 stack into locals buys nothing.** The stack is indexed by a run-time `top`,
  so the array stays in memory and every push and pop still goes through it. The gain (2.6-2.7x
  on a dot-product loop) comes only when the lifter tracks stack depth statically and gives each
  slot its own variable.
- **"`volatile` is not the problem" is true of MSVC only.** On GCC and Clang, the compilers the
  Linux/Vulkan port will use, dropping it takes the matrix multiply from 38.7 to 28.4 ns. But
  `volatile` is also load-bearing (MMIO fault decoding, guest spin loops), so the fix is to
  stop using scalar volatile accesses for packed XMM loads, not to remove it.
- **"`xbe_run` already selects exactly this class" is wrong.** `xbe_run` takes only framed
  functions with a plain `ret`, no string ops and no pointer arguments. None of the report's
  three hot functions qualify. The emitter work needs a differential A/B harness (old lifted C
  against new lifted C over the same guest state), which is what `tools/codegen_bench` does.
- **`g_xbox_mem_offset` is very likely not 0.** It is computed as `base - XBOX_MAP_START` with
  `XBOX_MAP_START == 0`, and Windows will not map below 64 KB. Nothing in the report depends on
  it.

Two cheap emitter changes are worth more than the report's "5-15%" estimate for its (d1) item:

- **A `rep stosd` fast path:** 12.7-18.6x on a 4 KB clear.
- **Packed-SSE helpers over intrinsics:** 1.9-2.0x on the matrix multiply, and 3.5-4.1x once its
  registers are cached too.

The report also misses a defect: every ALU instruction with a memory destination re-reads that
memory for its flag snapshot. Because the access is `volatile`, the re-read can never be
eliminated, and on a trapped MMIO register it is a second side-effecting read.

---

## 1. How this was checked, and what could not be

| Could check | Could not check |
| --- | --- |
| Every cited line of `recomp_types.h`, `lifter.py`, `translator.py`, the runtime, the HLE, MKDA's fibers, the CMake files and the conformance tools | MSVC instruction counts (151 / 916 / 313): no `cl` here |
| What the lifter emits, by lifting synthetic guest functions that have the same shape as the three hot functions (`tools/codegen_bench/snippets`) | OutRun 2's own lifted output: `src/recomp/gen` is ignored, and there is no XBE here |
| What GCC 13 and Clang 18 (`-O2`, x86-64) make of it, statically and timed | The profile in §3 of the report (`runs/vk_race3/sample.txt` is not in the repository); only its arithmetic was checked |

The snippets are not the OutRun 2 functions, so their counts are comparable to the report's in
kind, not in value. Timings come from an Intel Xeon at 2.1 GHz and are the best of five runs.
Run-to-run noise is about ±5%, so read any difference under 10% as no difference.

## 2. Claim by claim

✓ confirmed · ≈ right with a correction · ✗ wrong · ? not checkable here

### §1.1 The model

| Claim | | Evidence |
| --- | --- | --- |
| GPRs are `RECOMP_TLS` globals (`recomp_types.h` L163-172), aliased by `#define eax g_eax` (L1147-1153) | ✓ | As cited |
| `ebp` is a local seeded from `g_ebp`/`g_seh_ebp` and "re-published to `g_ebp` before every call" | ≈ | Re-published only when `publishes_ebp` is set (`_func_owns_a_frame`, `translator.py` ~L1318; `lifter.py` L2522). Frameless functions do not |
| x87 is `g_fp_stack[8]`/`g_fp_top` with per-function macros, and SSE/MMX are TLS unions | ✓ | L176-177, L480-481, L1184-1185; `FP_STACK_MACROS` in `translator.py` L306 |
| `XBOX_PTR` adds `g_xbox_mem_offset`, and MEM* are volatile casts | ✓ | L432-448 |
| The offset is "in practice 0" because the base view maps at the fixed base | ✗ (likely) | `g_memory_offset = base - XBOX_MAP_START` (`xbox_memory_layout.c` L1933). `XBOX_MAP_START` is 0 (`xbox_memory_layout.h` L41), the first placement tried is `0x00010000` (L1896), and Windows will not map below 64 KB, so the offset is at least `0x10000`. The startup line `mapped ... (offset +...)` settles it. No conclusion of the report depends on it, but "`_base == 0` as an optimisation" (§4.1) is then never taken |
| Call sites are `PUSH32(esp, ret); RECOMP_ABI_CALL_POP(...)` | ≈ | `RECOMP_ABI_CALL` (no `_POP`) when the callee's `ret N` is unknown, and `RECOMP_ICALL_SAFE` for targets in `recomp_manual.c` (`lifter.py` L2555-2583) |
| Flags are snapshotted into `_fa/_fb/_fas/_fbs` (and `double _fca/_fcb`); even the fused `cmp+jcc` emits and reads the snapshot | ✓ | `lift_basic_block` L4088-4101; `try_match_cmp_jcc` → `_make_condition` returns `_fa`/`_fas` for `cmp`/`test`. The comment at L4095, "the fused form tests the operands inline", is stale |
| `RECOMP_ICALL` makes three volatile trace writes, then manual → flat/binary → kernel lookup | ✓ | L992-1011; the flat table is in `translator.py` L2251-2289 |
| No trace hooks, ABI checks or MEM bounds checks in a plain lift | ✓ | Macros expand to plain calls; `RECOMP_ABI_CHECK` defaults OFF (`titles/outrun2/CMakeLists.txt` L53) |

### §1.2-1.5 Build flags and the hot functions

| Claim | | Evidence |
| --- | --- | --- |
| No optimisation flags in the title's CMake beyond CMake's `/O2 /Ob2`; `/bigobj /MP`; the kernel library gets `/O2 /GS- /Gy` | ✓ | `titles/outrun2/CMakeLists.txt` L130; `src/kernel/CMakeLists.txt` L62 |
| The `rep stosd` loop reloads `g_eax`, `g_edi`, `g_ecx` and the memory base for every dword: 10 instructions per store | ✓ (mechanism) | GCC gives the same 10-instruction loop with the same three TLS reloads. It does **not** reload `g_xbox_mem_offset`, because GCC uses type-based alias analysis and a `uint32_t` store cannot alias a `ptrdiff_t`. That is exactly the report's explanation for why MSVC does reload it |
| The 69-instruction SSE matrix multiply becomes 916 host instructions | ✓ (mechanism) | A 68-instruction equivalent becomes 536 (GCC) and 687 (Clang). Lane-wise helpers and TLS unions dominate |
| `comiss` costs two `cvtps2pd` because `_fca/_fcb` are `double` | ≈ | MSVC only. GCC and Clang fold `(double)a < (double)b` into `comiss` (0 conversions in the compiled snippet) |

### §2 Quantities and their interpretation

| Claim | | Evidence |
| --- | --- | --- |
| Flag snapshots are dead-store-eliminated: "free at run time" | ≈ | True when the destination is a register. **False when it is memory:** `_result_snapshot` emits `_fa = (uint32_t)(MEM32(dst))` *after* the write, and a volatile read cannot be removed even if `_fa` is dead. In the compiled call-loop snippet, `add [edi], eax` performs a load whose result is discarded. See N1 |
| TLS costs three instructions per function, not one per access; the L161 comment overstates it | ✓ | The comment is at L162. Nit: the TEB slot is `gs:[58h]`; MSVC listings print it as `gs:88` (decimal), which the report rendered as `gs:[88h]` |
| `volatile` is not the problem (identical counts with it removed) | ≈ | For MSVC, plausible and not re-checkable here. **GCC/Clang: it is.** Matrix multiply 38.7 → 28.4 ns (GCC), 45.2 → 37.0 ns (Clang); clear 778 → 566 ns (GCC). It also blocks merging the four dword loads of `XMM_MEM` into one. Removing it wholesale is unsafe (N4) |
| Dispatch is cheap; the ring-buffer writes are the only avoidable cost | ≈ | Cheap, yes (by the report's own table, 92% of call sites are direct). But the ring is read by the watchdog (`[WATCHDOG] ... recent ICALL targets`, `xbox_memory_layout.c` L1745), so "drop it in release" removes a hang diagnostic. The out-of-line `recomp_lookup_manual` call on every ICALL is also avoidable (N9) |

### §3 Profile

| Claim | | Evidence |
| --- | --- | --- |
| 39.1% lifted, 15.9% XDK DirectSound, 18.4% game, 4.7% XDK D3D/CRT, 5.8% `KiUserExceptionDispatcher` | ? | The data is not in the repo. The arithmetic is consistent: 1,065 + 1,232 + 316 = 2,613 of 6,687 samples. The exceptions under DirectSound fit the code: with `RECOMP_AC97_READY` on, the APU register window is `PAGE_NOACCESS` (`xbox_memory_layout.c` L2576), so every guest APU register access is a fault |
| ms figures by scaling on-CPU % by a 21 ms frame | ≈ | This assumes the guest thread is on CPU for the whole frame. Any time it blocks (GPU fence, flip gate) inflates every ms figure by the same factor. Re-measure after HLE audio, as the report itself says |

### §4 The rewrite

| Claim | | Evidence |
| --- | --- | --- |
| Option (i) touches HLE, the kernel bridge, `recomp_manual.c`, MKDA's fibers and the harness; the counts are 96 / 595 / 29 references | ≈ | Qualitatively right. The counts differ in this tree: `hle_d3d8.c` has 17 direct `g_e??` references (150 counting `HLE_ARG`/`HLE_RETURN`/`HLE_ORIGINAL`), `kernel_bridge.c` 650, OutRun 2's `recomp_manual.c` 12. Several cited line numbers are a few lines off. Probably measured on a different commit |
| Option (ii) changes nothing outside the generated files | ≈ | Mostly. But the runtime macros mix both spellings: `RECOMP_ICALL` writes `g_esp` and bare `eax`, and `RECOMP_ITAIL` writes `g_eax`. With `eax` a local, those macros need a sync around them. The emitter also writes `g_esp`, `g_ebp` and `g_seh_ebp` explicitly (`_icall_esp = g_esp`, the frame bridge). And the **watchdog** reads the game thread's register globals from another thread (`s_watchdog_regs`, ~L1708), as does the crash handler's stack walk (`templates/new-game/src/main.c` L226). Both become "as of the last call", which the report notes for watchpoints only |
| Fibers keep working because every switch happens inside a call to a manual override | ✓ | `titles/mkda/src/mk_tasks.c` sets `g_esp/g_ebx/g_esi/g_edi` and then `mk_call`s. Every switch is at a call boundary |
| The difference between (i) and (ii) is "small against what either removes"; (a') loses "a few %" at call-heavy code | ✗ (as built naively) | Measured: storing and reloading all seven GPRs at every call is 15-22% **slower** than today's globals on a one-call loop. Dirty- and liveness-aware sync is 21-32% faster. See N2 |
| x87 and XMM "should move with the integers" | ≈ | XMM yes (3.5-4.1x with intrinsic helpers). x87 no: moving `g_fp_stack` into a local array gave 0%. It needs static depth tracking (N3) |
| The conformance harness is 32-bit MSVC, so emitted C must stay x86-32 compatible | ✓ | `tools/conformance/__main__.py` L19. This also rules out putting `__m128` inside `RecompXmm`: a 16-byte-aligned union passed by value is rejected by 32-bit MSVC. `tools/codegen_bench` keeps the union and uses `_mm_loadu_ps`/`_mm_storeu_ps` in the helpers |

### §5-7 Options and plan

| Claim | | Evidence |
| --- | --- | --- |
| (c) leaf caching: "`xbe_run` already selects exactly this class as oracle candidates" | ✗ | `xbe_run` requires a `push ebp; mov ebp, esp` prologue, a plain `ret`, no string ops and only stack or absolute memory operands (`tools/conformance/xbe_run.py` docstring). All three hot functions fail: `rep stos`, `ret 12`/`ret 16`, FPO, pointer arguments |
| (d1) "5-15%" | ✗ (understated) | The `rep stosd` part alone is 12.7-18.6x on a 4 KB clear |
| (b) `/arch:AVX2` as an hours-long experiment | ≈ | Fine for the integer `MEM*` accesses, which stay legacy `mov`. But the MMIO decoder (`nv2a_mmio_hook.c`) knows only `mov`/`movzx`/`test`/`cmp`/`or`/`and`, and a vectorised copy into the DSP window has crashed it before (`vmovdqu`, `xbox_memory_layout.c` ~L2568). Run it with the APU trap on and watch the decode-failure count |
| "Host instructions per guest instruction is the right unit" | ≈ | Good for straight-line code, misleading for loops: the `stosd` fast path *adds* static instructions (148 → 161) and runs 18x faster. Time it |
| §7: the runtime items, not this one, get OutRun 2 to 60; this item buys the margin | ? | Plausible from the stated profile; not checkable here |

## 3. What the report missed

**N1. Memory-destination flag snapshots re-read memory.** For `add [m], r`, `dec [m]` and the
rest of the result-setting family, `_result_snapshot` (`lifter.py` L2040) reads the destination
back after writing it. Since `MEM32` is volatile, that read is always performed:

- **Cost:** one extra guest load per such instruction, even when no branch reads `_fa`. In the
  call-loop snippet, `dec [esp+0x14]; jnz` also re-reads the counter for the branch.
- **Correctness:** on a trapped MMIO register (APU, NV2A), the instruction becomes a write
  followed by a second read fault, so a read-side-effecting register is read twice.

Fix: compute the result once, `{ uint32_t _r = MEM32(a) + v; MEM32(a) = _r; _fa = _r; ... }`.
A small and local change to `_lift_alu_binop`/`_lift_inc_dec` and `_result_snapshot`.

**N2. Register caching needs liveness at calls, or it loses.** `caller.s` calls a leaf 100
times.

| sync at each call | GCC ns | Clang ns |
| --- | --- | --- |
| today (globals) | 448.7 | 362.1 |
| store all 7, reload all 7 | 513.9 (−15%) | 441.1 (−22%) |
| store dirty only, reload live only | 370.2 (+21%) | 274.4 (+32%) |

The emitter already builds the CFG and threads flag state through it. It needs two more
dataflow sets per instruction:

- **dirty** (forward, union at joins): written since the last sync.
- **live** (backward): read before being written.

Store `dirty` before a call, an ICALL, a tail jump or a fall-through, and at every return. Reload
`live` after a call. Conservatively treat every GPR as clobbered by an unknown callee. Treating
`ebx/esi/edi` as preserved is a later refinement that `RECOMP_ABI_CHECK` can police.

**N3. x87 needs static stack depth, not a local array.** The `top` index is a run-time value,
so `double _fps[8]` stays in memory and every `fld`/`faddp` round-trips through it with
store-to-load forwarding latency. Measured: 703 → 711 ns (no change). With the depth tracked by
the lifter and one `double` per slot pushed in the function: 703 → 257 ns (GCC), 705 → 274 ns
(Clang).

Compiler-generated x87 almost always has a static depth at every instruction; where joins
disagree, fall back to the array for that function. Values below the entry depth (arguments in
`st(0)`) and a returned `st(0)` cross calls through `g_fp_stack` at the sync points. Of the
1,123 OutRun 2 functions using x87, this is where they gain.

**N4. `volatile` is load-bearing; relax it only where an access cannot be MMIO or a spin
flag.** Two things depend on scalar `MEM*` being one exact-width access that is re-executed every
time:

- **The MMIO fault decoders**, which emulate the faulting host instruction and understand only a
  few integer forms.
- **Guest threads**, which now run on real host threads and spin on memory without barriers.

Packed XMM loads are neither, and a single 16-byte `movups` is closer to what `movaps` does than
four volatile dwords. So: keep scalar volatile, and do XMM memory with `_mm_loadu_ps`/`_mm_storeu_ps`
(N5). If a non-volatile scalar path is ever wanted, it has to come with a compiler barrier at
loop back-edges (the same place item 4's yield checks go), so that spin loops still reload.

**N5. The packed-SSE helpers can use intrinsics today without changing `RecompXmm`.**

- **Implementation:** wrap `_mm_loadu_ps(a.f)` / `_mm_storeu_ps(r.f, v)` around `_mm_add_ps`
  and the rest, and turn `XMM_SHUFFLE` into a macro (the lifter always passes a literal
  immediate). Keep the lane-wise C behind `#if` for non-x86 hosts.
- **Semantics:** the same as today's helpers. They are the instructions the guest executed:
  `minps`/`maxps` return the second operand on unordered or equal, `cmpneqps` is the unordered
  form, `andnps` is `~a & b`.
- **Measured:** matrix multiply 38.7 → 20.4 ns (GCC), 45.2 → 22.1 ns (Clang); with registers
  cached as well, 11.2 / 11.0 ns.

**N6. The `rep stosd`/`rep stosw` loops are the outliers among the string ops.** `rep stosb`
already uses `memset` and `rep movs*` already use `memcpy` when the ranges do not overlap, but
`stosd` is a per-dword loop that re-reads `ecx`, `eax` and `edi` (TLS) on every iteration.

- **Fix:** snapshot them into locals; when DF is clear and `eax` is one byte repeated (any
  clear), call `memset`.
- **Measured:** 4 KB clear 778 → 42 ns (GCC), 566 → 45 ns (Clang).

This needs neither the register rewrite nor a header change.

**N7. Validation needs an emitter A/B harness, and an MMIO regression run.**

- **The existing tools don't cover the hot set.** `tools.conformance` and `xbe_run` compare
  against the CPU, which is the right oracle for the lifter's *semantics*. But they need 32-bit
  MSVC and cannot run the hot set (above).
- **A/B covers what codegen changes need.** A pure codegen change has a better oracle: the
  current emitter's output. Run old and new C over the same guest state and compare registers
  and guest memory. That works for any function on any host. `tools/codegen_bench` is the
  smallest version of it, and the runner should grow from it.
- **Add an MMIO check.** Separately, any change to instruction selection should be followed by a
  title run with `RECOMP_AC97_READY=1` that checks the APU and NV2A decode-failure counters
  (`g_apu_mmio_decode_fail` in `apu_mmio_hook.c`) stay at zero.

**N8. The project's own docs say the opposite of the measurements.**

- `register-model.md` ("Compiler Optimization") says MSVC keeps globals in host registers within
  basic blocks and that LTCG/PGO help global access. Within a block, any guest store forces a
  reload, and LTCG cannot change that the store may alias.
- `ms-fusion-recompiler.md` item 4 ("Every `g_eax` touch is a TLS indirection") and the
  `recomp_types.h` L162 comment overstate the TLS cost.
- The `lift_basic_block` comment about the fused form is stale.

**N9. Smaller items.**

- **ICALL manual lookup.** `recomp_lookup_manual` is an out-of-line call on every ICALL. The
  lifter knows the manual set (`manual_scan`), so `recomp_dispatch_init` could pre-fill those
  VAs. It only matters if dispatch ever shows in a profile, and an override returned for a VA
  that `recomp_manual.c` does not define as `sub_` would have to keep the slow path.
- **TLS model on Linux.** The title executable can use `-ftls-model=initial-exec` (or
  `local-exec` with `-fno-pie`). That saves the GOT loads that pin TLS offsets in registers:
  148 → 135 instructions on the clear. TLS against plain globals and one TLS struct against
  seven variables both measured inside noise, which agrees with the report.
- **The `ICALL` ring and shared counters** are process-global and written by every guest thread.
  Per-thread copies, handed to the watchdog the way `s_watchdog_regs` are, would remove the
  cross-core traffic if audio and game threads ever both show dispatch in a profile.

## 4. Measurements

`python3 -m tools.codegen_bench` (ns per call, best of 5; host instructions in brackets). Every
build wrote identical guest memory.

| variant | cc | clear | matmul | keysearch | calls | x87sum |
| --- | --- | --- | --- | --- | --- | --- |
| base (today) | gcc | 777.7 [148] | 38.7 [536] | 22.0 [192] | 448.7 [82] | 703.1 [94] |
| base (today) | clang | 566.4 [120] | 45.2 [687] | 23.3 [204] | 362.1 [82] | 704.6 [81] |
| novol (attribution only) | gcc | 565.5 [143] | 28.4 [386] | 20.8 [166] | 457.0 [63] | 706.7 [88] |
| novol (attribution only) | clang | 577.3 [120] | 37.0 [568] | 22.7 [193] | 366.9 [76] | 710.6 [80] |
| stosd fast path (N6) | gcc | **41.8** [161] | | | | |
| stosd fast path (N6) | clang | **44.7** [193] | | | | |
| SSE intrinsics (N5) | gcc | | **20.4** [176] | | | |
| SSE intrinsics (N5) | clang | | **22.1** [206] | | | |
| locals, naive sync | gcc | 516.0 [114] | 34.6 [468] | 21.7 [158] | 513.9 [103] | 715.2 [127] |
| locals, naive sync | clang | 213.5 [165] | 30.0 [405] | 17.1 [145] | 441.1 [135] | 704.1 [126] |
| locals + intrinsics | gcc | | **11.2** [130] | | | |
| locals + intrinsics | clang | | **11.0** [142] | | | |
| liveness sync / static x87 (N2, N3) | gcc | | | | **370.2** [69] | **256.7** [59] |
| liveness sync / static x87 (N2, N3) | clang | | | | **274.4** [70] | **274.0** [52] |

Reading it:

- **The fast paths beat the register rewrite on the functions they cover.** The `stosd` fast
  path and the intrinsic helpers win more on their functions than register caching does, and
  neither touches the ABI.
- **Leaf register caching is worth 1.0-2.7x.** It is worth most when a loop's bound or value
  lives in a register (Clang's clear: 2.65x). GCC's keysearch, a short search loop, gains nothing
  measurable.
- **MSVC should gain more than this.** It reloads the memory base as well and folds no
  float-compare widening. Re-run the harness under `cl` on the build machine before setting
  checkpoints; the CMake-free build in `__main__.py` needs only the compiler name changed.

## 5. Recommended order

The report's order holds, with the cheap items moved ahead of everything else. The cheap items
are worth more than estimated, and liveness and x87 depth become part of the rewrite rather
than follow-ups.

1. **Tooling (days).** Grow `tools/codegen_bench` into an A/B runner over real lifted functions:
   - Lift a title twice (old and new emitter) and pick the hot set from a `RECOMP_SAMPLE` profile.
   - Run both versions over the same captured guest state.
   - Compare registers and guest memory.

   Add the MMIO decode-failure check (N7) to the title run.
2. **Emitter fast paths (days, no ABI change).**
   - `rep stosd`/`stosw` fast path (N6).
   - Packed-SSE helpers over intrinsics with the portable fallback (N5).
   - Memory-destination snapshots computed once (N1).
   - `_fca/_fcb` as `float` when the function has only `comiss`/`ucomiss` (MSVC only).
3. **Leaf caching (1-2 weeks)** behind a lifter flag. GPRs, XMM, `_base` and static-depth x87
   slots in one pass (N3). Leaves have no call sync to get wrong; they still need a sync before
   RECOMP_ICALL/ITAIL macros and tail jumps (the macros mix `g_` and bare names).
4. **All functions with dirty/live sync (3-5 weeks)** (N2). Keep a `--debug-spill` mode that
   syncs after every instruction for the watchdog, watchpoints and crash dumps. Gate on: A/B
   green on the hot set, MKDA fibers run, conformance green on x86-32, MMIO counters at zero.
5. **Then decide on the context struct** (report option (a)), with what step 4 measured.

Fix the docs in N8 along the way. They currently argue against step 3.

## 6. Reproducing

```bash
pip install capstone                               # binutils, gcc and/or clang assumed
python3 -m tools.codegen_bench                     # everything, ~1 minute
python3 -m tools.codegen_bench --cc clang --variants base,stosd
python3 -m tools.codegen_bench --show calls:liveness   # the C a variant compiles
```

`tools/codegen_bench/__main__.py` documents each variant: what it changes and why. The
`liveness` and static-x87 variants are hand-applied to their snippets to show what an emitter
with those analyses would write. They are measurements, not implementations.
