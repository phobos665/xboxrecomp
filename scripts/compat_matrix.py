#!/usr/bin/env python3
"""
Run every title and print one table of how far each one got.

The bring-up loop has always been one title at a time, and so has every
judgement about whether a change helped: the NtFreeVirtualMemory fix, the
LOCK-prefix flag fix and the XDK 3925 work were each validated against a
single game. "Did this regress Burnout 2" was not a question anyone could
answer without half an hour of manual runs, so it mostly went unasked.

This asks it. Each title is built, run headless for a fixed time, and reduced
to the handful of numbers that say where its frontier is:

    title            boot  device  swaps  draws  skipped  tex refused  icalls
    burnout2          yes     yes   1204   8817        0            0       0
    timesplitters2    yes     yes    903  12044       31            0       2
    maxpayne          yes     yes    122    239        0            0       2
    mkda              yes     yes      0      0        0            0       2
    ...

With --baseline it prints the same table against a previous run's JSON and
marks every column that moved, which is the actual question: not "how good is
this title" but "did what I just changed break one of the others".

    py -3 scripts/compat_matrix.py --refresh-thunks --build --seconds 45
    py -3 scripts/compat_matrix.py --baseline runs/matrix-before.json

Titles are discovered from titles/*/src/main.c, which records the game
directory and entry point. A title with no built executable is reported as
such rather than skipped, because "it stopped building" is a regression too.

These are windowed programs: a run opens a window per title, in sequence.
"""

import argparse
import json
import os
import re
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

CMAKE_CANDIDATES = [
    Path("C:/Program Files (x86)/Microsoft Visual Studio/2019/BuildTools/Common7"
         "/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe"),
    Path("cmake"),
]


def find_cmake():
    for c in CMAKE_CANDIDATES:
        if c.name == "cmake" or c.exists():
            return str(c)
    return "cmake"


def discover(only=None):
    """Every title project, with the game directory its main.c points at."""
    out = []
    for main_c in sorted((ROOT / "titles").glob("*/src/main.c")):
        name = main_c.parent.parent.name
        if only and name not in only:
            continue
        text = main_c.read_text(encoding="utf-8", errors="replace")
        m = re.search(r'YOUR_GAME_DIR\s+"([^"]*)"', text)
        game = None
        if m:
            game = m.group(1).replace("\\\\", "/").replace("\\", "/").split("games/")[-1]
        out.append({
            "name": name,
            "project": main_c.parent.parent,
            "game": game,
            "exe": main_c.parent.parent / "build" / "Release" / f"{name}_recomp.exe",
            "pipeline": ROOT / "games" / "_pipeline" / name / "out",
        })
    return out


def refresh_thunks(t, verbose=False):
    """Rewrite gen/recomp_hle.c against the current src/hle.

    Adding an HLE_EXPORT or HLE_ORIGINAL leaves every already-lifted title
    unable to link until its thunk file mentions the new name. Without this
    the matrix cannot run at all after an HLE change, which is exactly when
    it is most wanted.
    """
    funcs = t["pipeline"] / "disasm" / "functions.json"
    ident = t["pipeline"] / "func_id" / "identified_functions.json"
    xbe = ROOT / "games" / (t["game"] or "") / "default.xbe"
    syms = xbe.with_name("default_xdk_symbols.json")
    gen = t["project"] / "src" / "recomp" / "gen"
    if not (funcs.is_file() and xbe.is_file() and gen.is_dir()):
        return "no pipeline output"
    cmd = [sys.executable, "-m", "tools.recomp", str(xbe), "--game-only",
           "--split", "1000", "--functions", str(funcs),
           "--gen-dir", str(gen), "--only-hle-thunks"]
    if ident.is_file():
        cmd += ["--identified", str(ident)]
    if syms.is_file():
        cmd += ["--hle-symbols", str(syms)]
    manual = t["project"] / "src" / "recomp_manual.c"
    if manual.is_file():
        cmd += ["--exclude-manual", str(manual)]
    r = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
    if r.returncode != 0:
        return "refresh failed"
    m = re.search(r"(\d+) of (\d+) original bodies", r.stderr)
    return f"{m.group(1)}/{m.group(2)} bodies" if m else "refreshed"


def build(t):
    bdir = t["project"] / "build"
    if not (bdir / "CMakeCache.txt").is_file():
        return "not configured"
    r = subprocess.run([find_cmake(), "--build", str(bdir), "--config", "Release",
                        "--target", f"{t['name']}_recomp", "--", "-m", "-v:m"],
                       cwd=ROOT, capture_output=True, text=True)
    if r.returncode != 0:
        errs = [l for l in (r.stdout + r.stderr).splitlines()
                if " error " in l.lower()]
        return "BUILD FAILED: " + (errs[0][:90] if errs else "see log")
    return "built"


MARKERS = {
    "swaps":      (r"shadow: (\d+) swaps", max),
    "clears":     (r"shadow: \d+ swaps, (\d+) clears", max),
    "kernel":     (r"\[KERNEL\] summary: (\d+) total calls", max),
    "tex_binds":  (r"shadow textures: (\d+) binds", max),
    "tex_refused": (r"skipped \d+ not a texture, \d+ cube/volume, (\d+) format", max),
}

DRAW_RE = re.compile(
    r"draws: (\d+) UP \+ (\d+) indexed UP \+ (\d+) buffer \+ (\d+) indexed buffer drawn; "
    r"skipped (\d+) program without layout, (\d+) declaration shader, (\d+) unknown "
    r"shader, (\d+) stride, (\d+) primitive, (\d+) failed")


def summarise(err_text, exit_code):
    s = {k: 0 for k in MARKERS}
    for key, (pat, agg) in MARKERS.items():
        vals = [int(v) for v in re.findall(pat, err_text)]
        s[key] = agg(vals) if vals else 0
    drawn = skipped = 0
    for m in DRAW_RE.finditer(err_text):
        g = [int(x) for x in m.groups()]
        drawn = max(drawn, sum(g[:4]))
        skipped = max(skipped, sum(g[4:]))
    s["draws"] = drawn
    s["draws_skipped"] = skipped
    s["boot"] = "Loaded" in err_text and "sections" in err_text
    s["device"] = ("shadow device" in err_text
                   or "Direct3D_CreateDevice" in err_text)
    s["icalls"] = len(set(re.findall(r"unresolved (?:call |jump )?target 0x([0-9A-F]{8})",
                                     err_text)))
    s["exit"] = exit_code
    # Verdict, coarsest first: the point is to spot a title falling off a
    # rung, not to grade it.
    if s["swaps"] > 0:
        s["verdict"] = "renders"
    elif s["device"]:
        s["verdict"] = "device, no frames"
    elif s["boot"]:
        s["verdict"] = "boots"
    else:
        s["verdict"] = "no start"
    return s


def run_title(t, seconds, out_dir, extra_env=None):
    if not t["exe"].is_file():
        return {"verdict": "not built", "exit": None, "boot": False,
                "device": False, "swaps": 0, "draws": 0, "draws_skipped": 0,
                "tex_binds": 0, "tex_refused": 0, "icalls": 0, "kernel": 0,
                "clears": 0}
    env = dict(os.environ)
    env.setdefault("RECOMP_VBLANK", "1")
    env.setdefault("RECOMP_AC97_READY", "1")
    env.update(extra_env or {})
    err_path = out_dir / f"{t['name']}.err"
    with open(err_path, "wb") as errf:
        p = subprocess.Popen([str(t["exe"])], cwd=str(t["project"]),
                             stdout=subprocess.DEVNULL, stderr=errf, env=env)
        try:
            code = p.wait(timeout=seconds)
        except subprocess.TimeoutExpired:
            p.kill()
            p.wait()
            code = "timeout"
    text = err_path.read_text(encoding="utf-8", errors="replace")
    s = summarise(text, code)
    s["log"] = str(err_path.relative_to(ROOT))
    return s


COLUMNS = [("verdict", 18, None), ("swaps", 7, "higher"), ("draws", 8, "higher"),
           ("draws_skipped", 8, "lower"), ("tex_binds", 10, "higher"),
           ("tex_refused", 8, "lower"), ("icalls", 7, "lower"),
           ("kernel", 10, None)]


def print_table(rows, baseline=None):
    head = f"{'title':<16}" + "".join(f"{c:>{w}}" for c, w, _ in COLUMNS)
    print(head)
    print("-" * len(head))
    regressions = []
    for name, s in rows.items():
        line = f"{name:<16}"
        for col, w, better in COLUMNS:
            cur = s.get(col, 0)
            line += f"{str(cur):>{w}}"
            if baseline and name in baseline and better:
                was = baseline[name].get(col, 0)
                if isinstance(cur, int) and isinstance(was, int) and cur != was:
                    worse = (cur < was) if better == "higher" else (cur > was)
                    line += "!" if worse else "+"
                    if worse:
                        regressions.append(f"{name}.{col}: {was} -> {cur}")
                else:
                    line += " "
            elif baseline:
                line += " "
        if baseline and name in baseline:
            if s.get("verdict") != baseline[name].get("verdict"):
                line += f"   [was {baseline[name].get('verdict')}]"
                regressions.append(
                    f"{name}: {baseline[name].get('verdict')} -> {s.get('verdict')}")
        print(line)
    if baseline:
        print()
        if regressions:
            print("REGRESSED:")
            for r in regressions:
                print(f"  {r}")
        else:
            print("nothing regressed against the baseline")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--seconds", type=int, default=45,
                    help="how long to run each title (default 45)")
    ap.add_argument("--titles", help="comma-separated subset")
    ap.add_argument("--refresh-thunks", action="store_true",
                    help="rewrite each gen/recomp_hle.c first (needed after "
                         "any HLE_EXPORT or HLE_ORIGINAL change)")
    ap.add_argument("--build", action="store_true", help="build before running")
    ap.add_argument("--no-run", action="store_true",
                    help="refresh and/or build only")
    ap.add_argument("--out-dir", default="games/_pipeline/_matrix",
                    help="where per-title logs and the JSON go")
    ap.add_argument("--baseline", help="a previous run's JSON to compare against")
    args = ap.parse_args()

    only = set(args.titles.split(",")) if args.titles else None
    titles = discover(only)
    if not titles:
        print("no titles found under titles/*/src/main.c", file=sys.stderr)
        return 1
    out_dir = ROOT / args.out_dir
    out_dir.mkdir(parents=True, exist_ok=True)

    baseline = None
    if args.baseline:
        bp = Path(args.baseline)
        if not bp.is_absolute():
            bp = ROOT / bp
        if bp.is_file():
            baseline = json.loads(bp.read_text(encoding="utf-8")).get("titles")
        else:
            print(f"baseline {bp} not found; running without one", file=sys.stderr)

    rows = {}
    for t in titles:
        prefix = f"[{t['name']}]"
        if args.refresh_thunks:
            print(f"{prefix} thunks: {refresh_thunks(t)}", flush=True)
        if args.build:
            r = build(t)
            print(f"{prefix} build: {r}", flush=True)
            if r.startswith("BUILD FAILED"):
                rows[t["name"]] = {"verdict": "build failed", "swaps": 0,
                                   "draws": 0, "draws_skipped": 0,
                                   "tex_binds": 0, "tex_refused": 0,
                                   "icalls": 0, "kernel": 0, "note": r}
                continue
        if args.no_run:
            continue
        t0 = time.time()
        rows[t["name"]] = run_title(t, args.seconds, out_dir)
        print(f"{prefix} {rows[t['name']]['verdict']} "
              f"({time.time() - t0:.0f}s)", flush=True)

    if args.no_run:
        return 0

    print()
    print_table(rows, baseline)

    stamp = time.strftime("%Y%m%d-%H%M%S")
    jpath = out_dir / f"matrix-{stamp}.json"
    jpath.write_text(json.dumps({"seconds": args.seconds, "titles": rows},
                                indent=1), encoding="utf-8")
    latest = out_dir / "matrix-latest.json"
    latest.write_text(jpath.read_text(encoding="utf-8"), encoding="utf-8")
    print(f"\nwrote {jpath.relative_to(ROOT)} (and matrix-latest.json)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
