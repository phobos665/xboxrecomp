#!/usr/bin/env python3
"""
Summarise the frame-rate windows a run printed under RECOMP_FPS.

    py -3 scripts/fps_report.py runs/run12.err [runs/run13.err ...]

Each run gets one row: the configuration switches from its .meta, the number
of windows, the mean frame rate over the whole run (total swaps over total
time, not the mean of the windows), the median and best window, and the
delivered vblank rate. Frame rate swings with what is on screen -- an attract
movie, a menu, a loading screen -- so a single window is noise, and two runs
are compared on their run means with the same --seconds.

The first window is dropped by default: it starts at the first swap and holds
the boot's first frames, which are not representative. --keep-first keeps it.
"""

import argparse
import re
import statistics
import sys
from pathlib import Path

WINDOW = re.compile(
    r"^\[FPS\] t=\s*([0-9.]+)s\s+([0-9.]+) fps \((\d+) swaps\)\s+vblank\s+([0-9.]+) Hz"
    r" \| run mean ([0-9.]+) fps, vblank ([0-9.]+) Hz over ([0-9.]+)s", re.M)

# The shadow renderer's own five-second report, for a build without RECOMP_FPS.
SHADOW = re.compile(r"^\[HLE-D3D8\] shadow: (\d+) swaps,", re.M)


def summarise(err: Path, keep_first: bool) -> dict:
    text = err.read_text(encoding="utf-8", errors="replace")
    rows = WINDOW.findall(text)
    meta = err.with_suffix(".meta")
    switches = ""
    if meta.is_file():
        m = re.search(r"switches=(\S*)", meta.read_text(encoding="utf-8"))
        if m:
            switches = m.group(1)

    out = {"run": err.stem, "switches": switches, "windows": 0}
    if rows:
        if not keep_first and len(rows) > 1:
            rows = rows[1:]
        fps = [float(r[1]) for r in rows]
        hz = [float(r[3]) for r in rows]
        # The run mean from the last window is total swaps over total time
        # since the first swap, which is the number to compare runs on.
        last = rows[-1]
        out.update({
            "windows": len(rows),
            "mean_fps": float(last[4]),
            "median_fps": statistics.median(fps),
            "best_fps": max(fps),
            "worst_fps": min(fps),
            "vblank_hz": float(last[5]),
            "seconds": float(last[6]),
        })
        return out

    # No RECOMP_FPS lines: fall back to the shadow renderer's swap totals,
    # which arrive every five seconds of GetTickCount time.
    swaps = [int(s) for s in SHADOW.findall(text)]
    if len(swaps) >= 2:
        deltas = [(b - a) / 5.0 for a, b in zip(swaps, swaps[1:])]
        if not keep_first and len(deltas) > 1:
            deltas = deltas[1:]
        out.update({
            "windows": len(deltas),
            "mean_fps": sum(deltas) / len(deltas),
            "median_fps": statistics.median(deltas),
            "best_fps": max(deltas),
            "worst_fps": min(deltas),
            "vblank_hz": float("nan"),
            "seconds": 5.0 * len(deltas),
            "source": "shadow report (5 s windows, GetTickCount timed)",
        })
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("err", nargs="+", type=Path)
    ap.add_argument("--keep-first", action="store_true",
                    help="keep the first window (the boot's first frames)")
    args = ap.parse_args()

    print(f"{'run':<28} {'win':>3} {'mean':>6} {'median':>6} {'best':>6} "
          f"{'worst':>6} {'vblank':>7}  switches")
    for err in args.err:
        if not err.is_file():
            print(f"{err.stem:<28} missing", file=sys.stderr)
            continue
        s = summarise(err, args.keep_first)
        if not s["windows"]:
            print(f"{s['run']:<28} no [FPS] windows (run without RECOMP_FPS?)")
            continue
        print(f"{s['run']:<28} {s['windows']:>3} {s['mean_fps']:>6.1f} "
              f"{s['median_fps']:>6.1f} {s['best_fps']:>6.1f} {s['worst_fps']:>6.1f} "
              f"{s['vblank_hz']:>7.1f}  {s['switches']}"
              + (f"  ({s['source']})" if "source" in s else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
