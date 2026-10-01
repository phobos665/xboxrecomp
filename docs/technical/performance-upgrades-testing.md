# Testing the performance upgrades

*Branch `feat/performance-upgrades`, 1 Oct 2026. This is the procedure for
deciding whether the new code generation options can be turned on for a
title. The measurements behind them are in
[lifted-code-quality-review.md](lifted-code-quality-review.md).*

Nothing on this branch changes how a title runs until you ask for it. The
options are off by default, and a lift without them matches `main`
everywhere except two lifter bug fixes (below). The point of the test is to
earn the right to turn them on, one title at a time.

## What is on the branch

### The options

`--perf-opts` on `tools.recomp` / `scripts/recompile.py`, or the environment
variable `XBOXRECOMP_PERF_OPTS`. The variable is how a title's own build
script (e.g. TS2's `scripts/build.py`) passes the options through without
knowing about them. Values are `all`, `none`, or a comma list:

| option | changes | measured (gcc / clang, ns per call) |
| --- | --- | --- |
| `stosd` | `rep stosd/stosw` read ecx/eax/edi once; a forward clear below 0xF0000000 is a `memset` | 4 KB clear 504/377 → 32/30 |
| `rmw-snapshot` | `add [m], r` and the rest take their flag snapshot from the value written, not a second (volatile) read of guest memory | one load fewer per memory read-modify-write |
| `fcmp-float` | `comiss` snapshots stay `float` | MSVC only (gcc/clang already fold it) |
| `xmm-intrinsics` | packed-SSE helpers on host SSE intrinsics, in generated code only | matrix multiply 32/35 → 14/15 |
| `leaf-cache` | functions that call nothing keep guest registers in C locals | with all of the above: matrix multiply → 8.0/7.2, keyframe search 16/18.5 → 17/11 |

The four above `leaf-cache` are "the cheap set". In the commands below it is
written as:

```
CHEAP=stosd,rmw-snapshot,fcmp-float,xmm-intrinsics
```

### Two bug fixes that apply with or without the options

Both only change functions whose old C was invalid, so a title that works
should not notice.

- **`_flags` declaration.** A `repne scas`/`repe cmps` with no branch after
  it (the inline strlen idiom) used `_flags` without declaring it, so the
  function did not compile.
- **Shift counts.** `shl`/`shr` now mask their count to five bits, as x86
  does. A count of 32 or more was undefined behaviour in C, and gave the
  right answer only while the compiler could not see the count.

### The tools

| tool | what it answers | runs on |
| --- | --- | --- |
| `py -3 -m tools.conformance` | does the lifted C agree with the CPU? Now honours `XBOXRECOMP_PERF_OPTS` | Windows, 32-bit MSVC |
| `python3 -m tools.codegen_ab` | does the code lifted *with* the options agree with the code lifted *without* them, on the same guest machine? | Linux / WSL / macOS, gcc or clang |
| `python3 -m tools.codegen_bench` | how fast is each variant, per function? | Linux / WSL / macOS |
| `scripts/fps_report.py --after S` | frame rate of a run, counting only the windows after S seconds | anywhere |

## What was checked here, and what was not

Checked in the cloud session (Linux, gcc 13 and clang 18):

- With the options off, the lifter's output is byte-identical to `main` for
  1,679 functions (the conformance cases, the bench snippets and 1,500
  generated leaves). The only exception is the `_flags` fix.
- `tools.codegen_ab --opts all` agrees with the plain lift on every run, with
  both compilers and over several seeds (3,832 runs per seed). The only
  differences are which NaN a both-NaN packed op returns, which today's
  lifter already varies on between compilers.
- The same run on a synthetic XBE pushed through the real pipeline
  (`--xbe` mode).
- The two header builds of the packed-SSE helpers agree lane for lane on
  edge values (NaN, ±0, infinity, denormals).
- `py -3 -m pytest tools/recomp` gives the same 5 failures as `main`; all
  five predate this branch.

Not checked: **MSVC** (x64 or 32-bit), **any real title**, and **the MMIO
trap path**. Those are what tomorrow is for.

## 0. Get the branch

The toolkit (for OutRun 2, MKDA and the tools):

```bash
cd xboxrecomp
git fetch origin feat/performance-upgrades
git checkout feat/performance-upgrades
```

TimeSplitters 2. Point its submodule at the branch for the test, and do not
commit the pin until you decide to keep it:

```bash
cd split2-recomp/xboxrecomp
git fetch origin feat/performance-upgrades
git checkout FETCH_HEAD
cd ..
```

For the Linux tools, use WSL or any Linux box with the toolkit checked out:

```bash
sudo apt install gcc clang binutils python3-pip
pip install capstone pytest
```

## 1. Unit tests and the CPU oracle (Windows, ~10 minutes)

```bash
py -3 -m pip install pytest
py -3 -m pytest -q tools/recomp
```

Expect at most these five failures, all from before this branch (stale test
harnesses and one identifier only MSVC's CRT declares, so that one may pass
on Windows):

- `test_icall_guarded_runtime`
- `test_lifter_double_shift` (×2)
- `test_lifter_sar_width::test_sar_al_1_on_0x80`
- `test_reserved_idents` (onexit)

Then run the conformance harness three ways. The second run is also the
**only check that the intrinsics header compiles under 32-bit MSVC**:

```bash
py -3 -m tools.conformance > runs/conf_off.txt
XBOXRECOMP_PERF_OPTS=all py -3 -m tools.conformance > runs/conf_all.txt
XBOXRECOMP_PERF_OPTS=all py -3 -m tools.conformance --only corpus > runs/conf_all_corpus.txt
```

`conf_all` must report the same passes and failures as `conf_off`.

Then `xbe_run`. It runs each title's own leaf functions natively and
compares them with the lifted C, so with the variable set it checks leaf
caching against the CPU on real game code:

```bash
XBOXRECOMP_PERF_OPTS=all py -3 -m tools.conformance --xbe "games/Outrun 2/default.xbe" --xbe-limit 300
XBOXRECOMP_PERF_OPTS=all py -3 -m tools.conformance --xbe split2-recomp/game/default.xbe --xbe-limit 300
```

Compare each against the same command without the variable.

## 2. Old lift against new, on the title's own code (WSL, 5-45 minutes)

This needs the title's pipeline output: the `--work-dir` that
`scripts/recompile.py` was given. For TS2 that is `split2-recomp/.pipeline`;
for OutRun 2, `games/_pipeline/outrun2/out` (or wherever you lifted it).
From the toolkit checkout, with Windows paths under `/mnt/c/...`:

```bash
# The hottest functions first: a RECOMP_SAMPLE_DUMP from step 4 works as the list.
python3 -m tools.codegen_ab --xbe "/mnt/c/.../games/Outrun 2/default.xbe" \
    --work-dir "/mnt/c/.../games/_pipeline/outrun2/out" \
    --functions /mnt/c/.../runs/perf/or2_base.sample.txt --limit 300 --opts all

# Then everything the options change (OutRun 2: ~12,000 functions, about 45 minutes):
python3 -m tools.codegen_ab --xbe ... --work-dir ... --opts all --states 4

# The same for TS2:
python3 -m tools.codegen_ab --xbe /mnt/c/.../split2-recomp/game/default.xbe \
    --work-dir /mnt/c/.../split2-recomp/.pipeline --opts all --states 4
```

**Pass:** the last line is `A and B agree on every run.` Lines saying
`NaN payload only (not a failure)` are fine.

**Fail:** any `MISMATCH` line. It names the function, the state, the
registers or pages that differ, and the directory holding both builds' C
(`ab_a.c`, `ab_b.c`). Re-run with `--opts` set to one option at a time to
find which one, and keep that directory.

## 3. Build three executables per title (Windows)

Use the same commit and the same build machine for all three, so only the
options differ. Check that every lift log prints the line
`perf-opts: ...` with the set you meant.

**TimeSplitters 2** (from `split2-recomp/`, Git Bash):

```bash
mkdir -p runs/bin
for v in base cheap all; do
  case $v in
    base)  opts=none ;;
    cheap) opts=stosd,rmw-snapshot,fcmp-float,xmm-intrinsics ;;
    all)   opts=all ;;
  esac
  XBOXRECOMP_PERF_OPTS=$opts py -3 scripts/build.py --relift --from lift || break
  cp build/Release/split2_recomp.exe runs/bin/ts2_$v.exe
done
```

**OutRun 2** (from the toolkit, the way you normally lift it, then your
usual CMake build of `titles/outrun2`):

```bash
for v in base cheap all; do
  case $v in
    base)  opts=none ;;
    cheap) opts=stosd,rmw-snapshot,fcmp-float,xmm-intrinsics ;;
    all)   opts=all ;;
  esac
  py -3 scripts/recompile.py "games/Outrun 2/default.xbe" \
      --work-dir games/_pipeline/outrun2/out --project titles/outrun2 \
      --from lift --perf-opts $opts || break
  cmake --build titles/outrun2/build --config Release || break
  cp titles/outrun2/build/Release/outrun2_recomp.exe runs/bin/or2_$v.exe
done
```

Adjust the build directory if yours differs. `--from lift` is enough: the
options do not affect disassembly.

**Mortal Kombat: Deadly Alliance:** the `all` build only, as in step 5.

## 4. Benchmark runs

The rules, all of which matter:

- **Uncapped.** `RECOMP_FPS_CAP=0`. The flip gate is on by default and would
  hold every build at 60, hiding the difference.
- **Muted.** `RECOMP_MUTE=1`.
- **Fresh saves per run.** `RECOMP_SAVE_DIR=<new folder>`. This also moves
  UDATA, so TS2's scripted menu path stays the same.
- **The same input script**, the same length, and the same settings file for
  every build.
- **Interleaved.** base, cheap, all, base, cheap, all, base, cheap, all, so
  that heat and background load spread evenly.
- **No profiler in the timed runs.** Profile in separate runs.

**TimeSplitters 2** (from `split2-recomp/`, Git Bash). The script reaches
Siberia at about 70 s:

```bash
export RECOMP_MUTE=1 RECOMP_FPS=5 RECOMP_FPS_CAP=0
export RECOMP_GAME_DIR="$PWD/game"
export RECOMP_INPUT_SEQ="12000:start,16000:start,20000:start,24000:start,28000:a,34000:a,40000:a,46000:a,52000:a,58000:a,64000:a,70000:a"
mkdir -p runs/perf
for i in 1 2 3; do for v in base cheap all; do
  RECOMP_SAVE_DIR="$(mktemp -d)" py -3 xboxrecomp/scripts/run_and_report.py \
      runs/bin/ts2_$v.exe --seconds 150 --out-dir runs/perf --tag ts2_${v}_$i
done; done
py -3 xboxrecomp/scripts/fps_report.py --after 80 runs/perf/ts2_*.err
```

**OutRun 2:** the same shape, with the scripted race you used for
`runs/vk_race3` as `RECOMP_INPUT_SEQ`, `or2_$v.exe`, and `--after` set to
when the race starts.

**Profiles** (one run per build, separate from the timed ones):

```bash
for v in base all; do
  RECOMP_SAMPLE=250 RECOMP_SAMPLE_DUMP="$PWD/runs/perf/ts2_$v.sample.txt" \
  RECOMP_SAVE_DIR="$(mktemp -d)" py -3 xboxrecomp/scripts/run_and_report.py \
      runs/bin/ts2_$v.exe --seconds 150 --out-dir runs/perf --tag ts2_${v}_prof
done
```

Compare the guest main thread's split in the `[SAMPLE]` report: the lifted
code share should fall. The `base` dump is also the hot list for step 2.

### What to look for in every `all` and `cheap` log

All of these must hold:

- No `[APU] MMIO decode fail` or `[NV2A] MMIO decode fail` lines. These
  matter most: a change in how the compiler accesses guest memory shows up
  here first.
- No new `[THROW]`, `[INT3]`, `[EXIT]`, crash or watchdog lines compared with
  `base`.
- The same `[ICALL] unresolved` count as `base` (run_and_report prints it).
- The run reaches the same place. For TS2, the Siberia cutscene plays and
  the level loads. An F11 capture at the same moment in two builds should
  look the same.

## 5. Behaviour checks beyond the benchmark

- **Mortal Kombat: Deadly Alliance**, `all` build: one Arcade fight. Its
  tasks run on host fibers that swap the register globals by hand. Leaf
  caching should not interact with that (a fiber switch always happens
  inside a call, and cached functions make none), but it is the title most
  likely to show it if it does.
- **TS2 with your own pad**, `all` build: ten minutes of play, including the
  audio cutscene that showed the de-sync bug. `[audio-output] dropped=`
  should match `base`.

## 6. Reading the results

Expected:

- **Uncapped frame rate** up a few percent with `all`; more in scenes heavy
  on clears and packed SSE, nothing where the frame is spent outside lifted
  code. The review's estimate for the full register work (not on this
  branch) is 10-20%; leaf caching is part of that.
- **`cheap`** most of the gain in clear-heavy and SSE-heavy frames.
- **Correctness:** no difference at all.

Decide per title and per set. Turn a set on for a title when all of these hold:

- step 2 agrees;
- step 1's conformance results are unchanged;
- step 4 shows no decode failures and the same behaviour;
- the frame rate is not worse.

To turn it on, put the option in the title's build. For TS2, either export
`XBOXRECOMP_PERF_OPTS` in your shell, or add `--perf-opts` to the lift
command in `scripts/build.py`. Then commit the submodule pin. Making any
option the default for every title should wait until two titles have run
with it.

## If something breaks

1. **Find the option.** Rebuild with one option at a time
   (`XBOXRECOMP_PERF_OPTS=stosd`, ...).
2. **Find the function.** Run `tools.codegen_ab --xbe ... --opts <that one>`.
   A mismatch names the function, and its chunk directory has both builds'
   C.
3. **Debug with `leaf-cache` off.** The watchdog's register line, crash
   dumps and watchpoint reports read the register globals. Inside a cached
   function those show the values from its entry, not the current ones.
4. **If the MMIO path is involved,** the decode-failure line prints the host
   instruction bytes. Paste those along with the option name.
