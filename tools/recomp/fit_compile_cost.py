"""Refit the weights of translator.compile_cost from a real build.

--split-cost cuts the generated C into files of roughly equal estimated
compile cost, counted per function as labels and x87 stack accesses
(COST_PER_LABEL, COST_PER_X87 in translator.py). The weights were fitted on
Apple clang -O3. Another compiler may weigh the two differently; this measures
it from a clean build of a lifted title:

    py -3 -m tools.recomp.fit_compile_cost build/.ninja_log titles/ts2/src/recomp/gen
    python3 -m tools.recomp.fit_compile_cost a/.ninja_log a/gen b/.ninja_log b/gen ...

Each pair is a Ninja log and the gen/ directory that build compiled. Only the
first entry for each recomp_NNNN.c object is used, so a log holding a clean
build followed by rebuilds still works. Prints how well lines, labels and x87
accesses each explain the per-file compile time, then the least-squares fit
of time = c + a*labels + b*x87, and the integer ratio to put in translator.py.
"""

import argparse
import glob
import os
import re
import sys

_OBJECT = re.compile(r"(recomp_\d{4})\.c\.(?:o|obj)$")
_DEFINITION = re.compile(r"^void \w+\(void\)", re.M)


def ninja_times(path):
    """{recomp_NNNN: seconds} from the first build of each chunk in a .ninja_log."""
    times = {}
    with open(path, encoding="utf-8", errors="replace") as fh:
        for line in fh:
            parts = line.rstrip("\n").split("\t")
            if line.startswith("#") or len(parts) < 4:
                continue
            m = _OBJECT.search(parts[3])
            if m and m.group(1) not in times:
                times[m.group(1)] = (int(parts[1]) - int(parts[0])) / 1000.0
    return times


def file_features(path):
    """(lines, labels, x87 accesses) of one generated chunk."""
    with open(path, encoding="utf-8", errors="replace") as fh:
        text = fh.read()
    return (text.count("\n") + 1, text.count("\nloc_"), text.count("fp_"))


def _solve(rows, ys):
    """Least squares by the normal equations; rows already carry the 1."""
    k = len(rows[0])
    m = [[sum(r[i] * r[j] for r in rows) for j in range(k)]
         + [sum(r[i] * y for r, y in zip(rows, ys))] for i in range(k)]
    for c in range(k):
        p = max(range(c, k), key=lambda r: abs(m[r][c]))
        m[c], m[p] = m[p], m[c]
        if abs(m[c][c]) < 1e-12:
            continue
        for r in range(k):
            if r != c:
                f = m[r][c] / m[c][c]
                m[r] = [a - f * b for a, b in zip(m[r], m[c])]
    return [m[i][k] / m[i][i] if abs(m[i][i]) > 1e-12 else 0.0
            for i in range(k)]


def _r2(ys, pred):
    mean = sum(ys) / len(ys)
    total = sum((y - mean) ** 2 for y in ys)
    return 1 - sum((y - p) ** 2 for y, p in zip(ys, pred)) / total if total else 0.0


def fit(samples):
    """samples: [(seconds, (lines, labels, x87))]. Returns a report string."""
    ys = [s for s, _ in samples]
    out = [f"{len(samples)} files, {sum(ys):.0f} s of compile time"]
    for i, name in enumerate(("lines", "labels", "x87 accesses")):
        rows = [(1.0, f[i]) for _, f in samples]
        w = _solve(rows, ys)
        pred = [w[0] + w[1] * f[i] for _, f in samples]
        out.append(f"  {name:13s} alone: R^2 {_r2(ys, pred):.2f}")
    rows = [(1.0, f[1], f[2]) for _, f in samples]
    c, a, b = _solve(rows, ys)
    pred = [c + a * f[1] + b * f[2] for _, f in samples]
    out.append(f"  labels + x87:       R^2 {_r2(ys, pred):.2f}  "
               f"({a * 1000:.3f} ms a label, {b * 1000:.3f} ms an x87 access, "
               f"{c:.2f} s a file)")
    if a > 0 and b > 0:
        out.append(f"  COST_PER_LABEL : COST_PER_X87 = {a / b:.2f} : 1")
    return "\n".join(out)


def main(argv=None):
    ap = argparse.ArgumentParser(
        description=__doc__.split("\n\n")[0],
        epilog="Pairs of: NINJA_LOG GEN_DIR")
    ap.add_argument("pairs", nargs="+", metavar="LOG_OR_GEN")
    args = ap.parse_args(argv)
    if len(args.pairs) % 2:
        ap.error("give a .ninja_log and a gen/ directory for each build")
    samples = []
    for log, gen in zip(args.pairs[::2], args.pairs[1::2]):
        times = ninja_times(log)
        for path in sorted(glob.glob(os.path.join(gen, "recomp_[0-9][0-9][0-9][0-9].c"))):
            key = os.path.basename(path)[:-2]
            if key in times:
                samples.append((times[key], file_features(path)))
    if len(samples) < 3:
        print("not enough compiled chunks matched the logs", file=sys.stderr)
        return 1
    print(fit(samples))
    return 0


if __name__ == "__main__":
    sys.exit(main())
