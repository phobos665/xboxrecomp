#!/usr/bin/env python3
"""
Audit Xbox kernel ordinal coverage.

Counts what the kernel layer actually implements, by reading the C sources
rather than trusting a number written in a README. Three different tables have
to agree before a kernel import works at runtime:

  xbox_resolve_ordinal()        kernel_thunks.c  host-side function pointers
  bridge_for_ordinal()          kernel_bridge.c  argument-marshalling bridges
  kernel_data_va_for_ordinal()  kernel_bridge.c  DATA exports (not functions)

Translated game code reaches the kernel *only* through the bridge tables. An
ordinal that has a resolve_ordinal entry but no bridge is not implemented in
any useful sense: kernel_thunk_dispatch() logs a warning and returns 0 in eax,
which the game sees as a successful call with a zero result.

Usage:
    python3 scripts/audit_kernel_ordinals.py            # human-readable report
    python3 scripts/audit_kernel_ordinals.py --list-gaps
    python3 scripts/audit_kernel_ordinals.py --json
"""

import argparse
import json
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent

# Total ordinal slots in the Xbox kernel export table.
TOTAL_ORDINALS = 366

CASE_RE = re.compile(r"^\s*case\s+(\d+)\s*:", re.M)


def cases_in_function(path: Path, signature: str) -> set:
    """Return the set of `case N:` labels inside one C function body.

    Scans from the function signature to the first line that is exactly `}`,
    which is the closing brace of a top-level function in this codebase.
    """
    text = path.read_text(encoding="utf-8", errors="replace")
    start = text.find(signature)
    if start == -1:
        raise SystemExit(f"error: {signature!r} not found in {path}")

    end = text.find("\n}\n", start)
    if end == -1:
        raise SystemExit(f"error: could not find end of {signature!r} in {path}")

    return {int(m) for m in CASE_RE.findall(text[start:end])}


def collect():
    thunks = REPO / "src" / "kernel" / "kernel_thunks.c"
    bridge = REPO / "src" / "kernel" / "kernel_bridge.c"

    implemented = cases_in_function(thunks, "ULONG_PTR xbox_resolve_ordinal(")
    bridges = cases_in_function(bridge, "static bridge_func_t bridge_for_ordinal(")
    data = cases_in_function(bridge, "static uint32_t kernel_data_va_for_ordinal(")

    reachable = bridges | data
    return {
        "implemented": implemented,
        "bridges": bridges,
        "data_exports": data,
        "reachable": reachable,
        # Implemented host-side but unreachable from translated code.
        "unbridged": implemented - reachable,
        # Bridged but absent from the host-side table.
        "orphan_bridges": reachable - implemented,
        "unimplemented": set(range(1, TOTAL_ORDINALS + 1)) - (implemented | reachable),
    }


def fmt(ordinals) -> str:
    return " ".join(str(o) for o in sorted(ordinals))


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--list-gaps", action="store_true",
                    help="list the ordinals in each gap category")
    ap.add_argument("--json", action="store_true", help="machine-readable output")
    args = ap.parse_args()

    r = collect()

    if args.json:
        print(json.dumps({k: sorted(v) for k, v in r.items()}, indent=2))
        return 0

    print("Xbox kernel ordinal coverage")
    print("=" * 60)
    print(f"  Total ordinal space                  {TOTAL_ORDINALS}")
    print(f"  Reachable from translated code       {len(r['reachable'])}")
    print(f"    - function bridges                 {len(r['bridges'])}")
    print(f"    - data exports                     {len(r['data_exports'])}")
    print(f"  Host-side implementations            {len(r['implemented'])}")
    print()
    print(f"  Implemented but NOT bridged          {len(r['unbridged'])}")
    print("      These return 0 to the game instead of doing anything.")
    print(f"  Bridged but not in resolve_ordinal   {len(r['orphan_bridges'])}")
    print(f"  No implementation at all             {len(r['unimplemented'])}")

    if args.list_gaps:
        print()
        print("Implemented but not bridged:")
        print("  " + fmt(r["unbridged"]))
        print()
        print("Bridged but missing from xbox_resolve_ordinal:")
        print("  " + fmt(r["orphan_bridges"]))

    print()
    print("Note: coverage is driven by what titles actually call, not by XDK")
    print("version. A number here is a property of the toolkit, never of one game.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
