"""Old lifter output against new, over the same guest machine.

    python3 -m tools.codegen_ab                         # corpus, --perf-opts all
    python3 -m tools.codegen_ab --opts leaf-cache --states 32
    python3 -m tools.codegen_ab --xbe games/ts2/default.xbe \\
        --work-dir games/_pipeline/ts2/out --functions hot.txt

A perf option changes how an instruction is spelled, never what it does, so
the oracle for one is the lifter without it. Each function is lifted twice --
A with no perf options, B with --opts -- and both builds run from identical
seeded machines; every register, every changed guest page, the machine state
at every call, and how the run ended (return, fault, timeout) must agree.

That is a different question from tools.conformance, which asks whether the
lifter agrees with the CPU and needs 32-bit MSVC to ask it. This needs gcc or
clang and a POSIX host (Linux, WSL, macOS), and it covers what conformance's
candidate rules exclude: frameless functions, `ret N`, pointer arguments,
string ops -- which is where the hot functions are.

Corpus mode lifts the conformance cases, the codegen_bench snippets and
seeded random leaf functions. --xbe lifts a title's own functions from its
pipeline output (scripts/recompile.py --work-dir) and maps the title's
sections into the guest machine, so globals read real initial data. Callees
are stubs that record the call and return, in both builds alike.

Exit status 0 only when every run agreed. Mismatches are listed with the
fields that differ and the C for both builds is kept in --workdir.
"""

import argparse
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile

from ..recomp import config, perf_opts
from ..recomp.translator import FunctionTranslator, perf_defines
from . import corpus

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
RUNTIME = os.path.join(ROOT, "templates", "runtime")

_DEF = re.compile(r"^(void|uint32_t|int|float) ([A-Za-z_][A-Za-z0-9_]*)\(void\)$",
                  re.M)
_VA_SUFFIX = re.compile(r"([0-9A-Fa-f]{8})$")


# ── lifting ──────────────────────────────────────────────────────────────

def lift_snippet(name, va, code, opts):
    """One corpus snippet, alone in its own image."""
    config._install([config.Section(".text", va, len(code), 0, len(code), True)],
                    entry_point=va, kernel_thunk_addr=va, origin="codegen_ab")
    db = {va: {"start": f"0x{va:08X}", "end": va + len(code), "_addr": va,
               "size": len(code)}}
    t = FunctionTranslator(code, db, perf_opts=opts)
    c = t.translate_function(va, db[va])
    return c, dict(t.lifter.referenced_calls)


def corpus_lifts(args, workdir):
    funcs, skipped = corpus.build(workdir, generated=args.generated,
                                  seed=args.seed,
                                  sources=args.sources.split(","))
    for name, why in skipped:
        print(f"  skipped {name}: {why}", file=sys.stderr)
    out = []
    for name, va, code in funcs:
        a, ra = lift_snippet(name, va, code, frozenset())
        b, rb = lift_snippet(name, va, code, args.opts)
        if a and b:
            out.append((name, va, a, b, {**ra, **rb}))
    return out, None


def xbe_lifts(args, workdir):
    """A title's functions, lifted the way scripts/recompile.py lifts them."""
    from ..recomp.__main__ import find_data_files
    from ..recomp.translator import BatchTranslator

    work = args.work_dir
    data = find_data_files(
        disasm_dir=os.path.join(work, "disasm") if work else None,
        func_id_dir=os.path.join(work, "func_id") if work else None,
        abi_dir=None, overrides={"functions": None, "labels": None,
                                 "identified": None, "abi": None})
    if not data.get("functions"):
        raise SystemExit("functions.json not found: pass --work-dir, the "
                         "--work-dir scripts/recompile.py was given")
    config.configure_from_xbe(args.xbe)

    def batch(opts):
        return BatchTranslator(
            xbe_path=args.xbe, func_json_path=data["functions"],
            labels_json_path=data.get("labels"),
            identified_json_path=data.get("identified"),
            abi_json_path=data.get("abi"), output_dir=workdir,
            perf_opts=opts)

    bt_a, bt_b = batch(frozenset()), batch(args.opts)
    if args.functions:
        with open(args.functions) as f:
            wanted = [int(tok, 16) for line in f
                      for tok in re.findall(r"(?:0x)?([0-9A-Fa-f]{6,8})\b",
                                            line.split("#")[0])[:1]]
    else:
        wanted = sorted(bt_a.func_db)
    if args.limit:
        wanted = wanted[:args.limit]
    out = []
    for va in wanted:
        info = bt_a.func_db.get(va)
        if not info:
            print(f"  0x{va:08X}: not a function in functions.json", file=sys.stderr)
            continue
        a = bt_a.translator.translate_function(va, info)
        b = bt_b.translator.translate_function(va, bt_b.func_db[va])
        if a and b and (a != b or args.include_identical):
            out.append((f"sub_{va:08X}", va, a, b, {}))
    refs = {**bt_a.translator.lifter.referenced_calls,
            **bt_b.translator.lifter.referenced_calls}
    out = [(n, va, a, b, refs) for n, va, a, b, _ in out]

    # The title's sections, as the guest machine's initial memory.
    image = os.path.join(workdir, "image.bin")
    with open(args.xbe, "rb") as f:
        xbe = f.read()
    with open(image, "wb") as f:
        for s in config._SECTIONS:
            raw = xbe[s.raw_addr:s.raw_addr + min(s.raw_size, s.va_size)]
            f.write(struct.pack("<II", s.va, len(raw)))
            f.write(raw)
    return out, image


# ── building ─────────────────────────────────────────────────────────────

def write_build(lifts, opts, workdir):
    """ab_a.c, ab_b.c and ab_stubs.c for the runner."""
    refs = {}
    for _, _, _, _, r in lifts:
        refs.update({name: va for va, name in r.items()})
    decls = "".join(f"void {n}(void);\n" for n in sorted(refs))
    for side, which, defines in (("a", 2, []), ("b", 3, perf_defines(opts))):
        body, table = [], []
        for i, item in enumerate(lifts):
            c = item[which]
            m = _DEF.search(c)
            if not m:
                raise SystemExit(f"{item[0]}: no function definition found")
            fn = f"ab_{side}_{i}"
            body.append(c[:m.start()] + f"void {fn}(void)" + c[m.end():])
            table.append(f'    {{"{item[0]}", 0x{item[1]:08X}u, {fn}}},')
        src = ["#define RECOMP_GENERATED_CODE", *defines,
               '#include "recomp_types.h"', "#include <math.h>",
               "#include <string.h>", decls] + body + [
               "typedef struct { const char *name; uint32_t va; void (*fn)(void); } AbEntry;",
               f"const AbEntry ab_table_{side}[] = {{"] + table + ["};"]
        if side == "a":
            src.append(f"const int ab_count = {len(lifts)};")
        with open(os.path.join(workdir, f"ab_{side}.c"), "w") as f:
            f.write("\n".join(src) + "\n")
    stubs = ["#include <stdint.h>", "void ab_stub(uint32_t va);"]
    for name, va in sorted(refs.items()):
        stubs.append(f"void {name}(void) {{ ab_stub(0x{va:08X}u); }}")
    with open(os.path.join(workdir, "ab_stubs.c"), "w") as f:
        f.write("\n".join(stubs) + "\n")


def build(cc, workdir, opt_flags):
    exe = os.path.join(workdir, "ab_runner")
    # -fwrapv and -fno-strict-aliasing: the title builds are MSVC, which does
    # neither signed-overflow nor type-based alias optimisation. Without them
    # gcc/clang may legitimately compile the two variants of UB-adjacent
    # lifted C differently, and the runner would report the compiler, not
    # the lifter.
    flags = [opt_flags, "-fwrapv", "-fno-strict-aliasing", "-w", "-I", RUNTIME]
    srcs = [os.path.join(HERE, "runner.c")] + [
        os.path.join(workdir, f) for f in ("ab_a.c", "ab_b.c", "ab_stubs.c")]
    r = subprocess.run([cc] + flags + ["-o", exe] + srcs + ["-lm"],
                       capture_output=True, text=True)
    if r.returncode:
        sys.stderr.write(r.stderr[-6000:])
        raise SystemExit(f"{cc}: the A/B build failed (sources in {workdir})")
    return exe


# ── comparing ────────────────────────────────────────────────────────────

def _fields(line):
    if not line.startswith("OK "):
        return {"outcome": line.strip()}
    out = {"outcome": "OK"}
    for tok in line.split()[1:]:
        k, _, v = tok.partition("=")
        out[k] = v
    return out


def parse_runs(text):
    """{(fn, state): (name, A fields, B fields, A pages, B pages)}"""
    runs, cur = {}, None
    lines = iter(text.splitlines())
    for line in lines:
        if line.startswith("RUN "):
            _, fi, st, name = line.split(" ", 3)
            cur = [int(fi), int(st), name, None, None, "", ""]
            runs[(cur[0], cur[1])] = cur
        elif line.startswith(("A ", "B ")) and cur is not None:
            side = 3 if line[0] == "A" else 4
            cur[side] = _fields(line[2:])
            if line[2:].startswith("OK "):
                pages = next(lines, "")
                cur[side + 2] = pages
        elif line.startswith("CRASH") and cur is not None:
            side = 4 if cur[4] is not None else 3
            cur[side] = {"outcome": line.strip()}
    return runs


def compare(a, b):
    """Fields that differ, or None when the run says nothing.

    Only runs where A -- today's lifter -- returns normally are compared. If A
    faults, the title would already have crashed there, and what B does past
    that point cannot break a title that works: B may legitimately not fault
    (a packed load whose result is dead is a volatile access in A and may be
    dropped in B) or fault at a different address (a memset touches the
    unmapped page in a different order than an element loop). If A times out,
    B finishing first -- a 2 GB clear done by memset -- is the point of the
    option, not a difference. B faulting or timing out where A returned is a
    mismatch."""
    if a is None:
        return ["missing result"]
    if a["outcome"] != "OK":
        return None
    if b is None:
        return ["missing result for B"]
    if a["outcome"] != b["outcome"]:
        return [f"outcome {a['outcome']} vs {b['outcome']}"]
    return [f"{k} {a.get(k)} vs {b.get(k)}" for k in a if a.get(k) != b.get(k)]


# ── NaN payloads ─────────────────────────────────────────────────────────
#
# When both lanes of a packed op are NaN, x86 returns the first operand's;
# the lane-wise helpers are C float arithmetic, which the compiler may
# commute, so which NaN comes out depends on how it ordered the operands.
# That is true of today's lifter between compilers and between call sites,
# and any change to the code around an op can flip it. A difference made only
# of values that are NaN in both builds is reported apart from the rest.

def _nan32(v):
    return (v >> 23) & 0xFF == 0xFF and v & 0x7FFFFF != 0


def _nan64(v):
    return (v >> 52) & 0x7FF == 0x7FF and v & ((1 << 52) - 1) != 0


def nan_only_registers(a, b, keys):
    for k in keys:
        va, vb = a.get(k, ""), b.get(k, "")
        if k.startswith("xmm"):
            for i in range(0, 32, 8):
                x, y = int(va[i:i + 8], 16), int(vb[i:i + 8], 16)
                if x != y and not (_nan32(x) and _nan32(y)):
                    return False
        elif k.startswith("st"):
            if not (_nan64(int(va, 16)) and _nan64(int(vb, 16))):
                return False
        else:
            return False
    return True


def nan_only_pages(da, db):
    """da/db: {page address: bytes}. True if every differing dword (or the
    qword around it) is a NaN in both."""
    for page in set(da) | set(db):
        x, y = da.get(page), db.get(page)
        if x is None or y is None:
            return False
        for off in range(0, len(x), 4):
            if x[off:off + 4] == y[off:off + 4]:
                continue
            if _nan32(struct.unpack_from("<I", x, off)[0]) and \
               _nan32(struct.unpack_from("<I", y, off)[0]):
                continue
            q = off & ~7
            if _nan64(struct.unpack_from("<Q", x, q)[0]) and \
               _nan64(struct.unpack_from("<Q", y, q)[0]):
                continue
            return False
    return True


def dumps_for(exe, fi, states, timeout, image, seed):
    """{state: (A pages, B pages)} for one function, from a dump run."""
    r = subprocess.run([exe, str(states), str(timeout), str(fi), image or "",
                        str(seed), "1"], capture_output=True, text=True)
    out, st, side = {}, None, None
    for line in r.stdout.splitlines():
        if line.startswith("RUN "):
            st = int(line.split()[2])
            out[st] = ({}, {})
        elif line.startswith("A "):
            side = 0
        elif line.startswith("B "):
            side = 1
        elif line.startswith("DUMP ") and st is not None and side is not None:
            _, addr, hexs = line.split(" ", 2)
            out[st][side][addr] = bytes.fromhex(hexs.strip())
    return out


def page_diff(pa, pb):
    da = dict(t.split(":") for t in pa.split()[1:])
    db = dict(t.split(":") for t in pb.split()[1:])
    return sorted(k for k in set(da) | set(db) if da.get(k) != db.get(k))


def main(argv=None):
    ap = argparse.ArgumentParser(prog="python3 -m tools.codegen_ab",
                                 description=__doc__.split("\n\n")[0])
    ap.add_argument("--opts", default="all",
                    help="perf options for build B (A has none): 'all' or a "
                         "comma list of " + ", ".join(perf_opts.OPTS))
    ap.add_argument("--states", type=int, default=8,
                    help="seeded machines per function")
    ap.add_argument("--cc", default="gcc")
    ap.add_argument("--opt", default="-O2")
    ap.add_argument("--timeout", type=int, default=1, help="seconds per run")
    ap.add_argument("--generated", type=int, default=300,
                    help="random leaf functions in the corpus")
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--sources", default="conformance,bench,generated")
    ap.add_argument("--xbe", help="lift this title's functions instead")
    ap.add_argument("--work-dir", help="the title's pipeline output "
                    "(scripts/recompile.py --work-dir)")
    ap.add_argument("--functions", help="file of function VAs (hex, first on "
                    "each line), e.g. the hot list from a RECOMP_SAMPLE run")
    ap.add_argument("--limit", type=int, default=0)
    ap.add_argument("--include-identical", action="store_true",
                    help="with --xbe, also run functions the options leave "
                         "unchanged (skipped by default: nothing to compare)")
    ap.add_argument("--workdir", default=os.path.join(HERE, "build"))
    ap.add_argument("--keep-going", action="store_true")
    args = ap.parse_args(argv)
    try:
        args.opts = perf_opts.parse(args.opts)
    except ValueError as exc:
        ap.error(str(exc))
    if os.name != "posix":
        raise SystemExit("tools.codegen_ab needs a POSIX host (Linux, WSL, macOS)")
    for tool in (args.cc, "as", "ld", "objcopy"):
        if not shutil.which(tool):
            raise SystemExit(f"{tool} not found")

    os.makedirs(args.workdir, exist_ok=True)
    print(f"B: --perf-opts {perf_opts.describe(args.opts)}; A: none", file=sys.stderr)
    lifts, image = (xbe_lifts if args.xbe else corpus_lifts)(args, args.workdir)
    changed = sum(1 for _, _, a, b, _ in lifts if a != b)
    print(f"{len(lifts)} functions lifted both ways, {changed} differ in the C",
          file=sys.stderr)
    if not lifts:
        return 0
    write_build(lifts, args.opts, args.workdir)
    exe = build(args.cc, args.workdir, args.opt)
    r = subprocess.run([exe, str(args.states), str(args.timeout), "-1",
                        image or "", str(args.seed)], capture_output=True, text=True)
    with open(os.path.join(args.workdir, "runs.txt"), "w") as f:
        f.write(r.stdout)
    runs = parse_runs(r.stdout)

    bad, nan_only, outcomes = [], [], {}
    dump_cache = {}
    for (fi, st), (_, _, name, fa, fb, pa, pb) in sorted(runs.items()):
        key = (fa or {}).get("outcome", "?").split()[0]
        outcomes[key] = outcomes.get(key, 0) + 1
        diffs = compare(fa, fb)
        if diffs is None:
            outcomes["not compared (A did not return)"] = \
                outcomes.get("not compared (A did not return)", 0) + 1
        elif diffs:
            keys = [d.split()[0] for d in diffs]
            reg_keys = [k for k in keys if k != "pages"]
            nan = reg_keys == [] or nan_only_registers(fa, fb, reg_keys)
            if nan and "pages" in keys:
                if fi not in dump_cache:
                    dump_cache[fi] = dumps_for(exe, fi, args.states, args.timeout,
                                               image, args.seed)
                da, db = dump_cache[fi].get(st, ({}, {}))
                nan = nan_only_pages(da, db)
            if any(d.startswith("pages") for d in diffs) and pa and pb:
                diffs.append("pages differing: " + " ".join(page_diff(pa, pb)[:8]))
            (nan_only if nan else bad).append((name, st, diffs))
    expected = len(lifts) * args.states
    print(f"{len(runs)} of {expected} runs: " +
          ", ".join(f"{k} {v}" for k, v in sorted(outcomes.items())))
    if len(runs) != expected:
        print("runner stopped early; see " + os.path.join(args.workdir, "runs.txt"))
        bad.append(("(runner)", -1, ["incomplete"]))
    for name, st, diffs in nan_only[:10]:
        print(f"NaN payload only (not a failure) {name} state {st}: "
              + "; ".join(diffs))
    if nan_only:
        print(f"{len(nan_only)} runs differ only in which NaN a both-NaN op "
              f"returned; see the note above nan_only_registers()")
    for name, st, diffs in bad[:40]:
        print(f"MISMATCH {name} state {st}: " + "; ".join(diffs))
    if bad:
        print(f"\n{len(bad)} mismatching runs. Both builds' C: "
              f"{args.workdir}/ab_a.c, ab_b.c")
        return 1
    print("A and B agree on every run.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
