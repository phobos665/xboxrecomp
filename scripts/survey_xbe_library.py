#!/usr/bin/env python3
"""
Survey a library of Xbox titles and rank them as recompilation targets.

LTCG status and XDK build cannot be looked up on a wiki. LTCG is a per-build
linker setting recorded nowhere public, and the XDK build is a property of the
disc, not of the title. Both are in the XBE header, so read them.

Reports per title: XDK version, D3D8 vs D3D8LTCG, the library list, RenderWare
detection and version, .text size, kernel import count, and demand-loaded
sections.

Usage:
    python3 scripts/survey_xbe_library.py /path/to/extracted/discs --csv survey.csv
    python3 scripts/survey_xbe_library.py /path/to/one/default.xbe --verbose

The ranking is a starting point, not a verdict. It cannot see hand-rolled push
buffers, threading complexity, or whether you actually want to play the game.
"""

import argparse
import csv
import re
import sys
from pathlib import Path

# Import the XBE parser from the toolkit rather than re-deriving header offsets.
sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from tools.xbe_parser.xbe_parser import (  # noqa: E402
    XBEParser,
    SECTION_PRELOAD,
)

# Sections whose presence signals work the toolkit does not yet cover.
COSTLY_SECTIONS = {
    "WMADEC": "WMA audio decoding is not implemented",
    "XONLINE": "Xbox Live code path",
}

COSTLY_LIBRARIES = {
    "XONLINE": "Xbox Live",
    "XONLINES": "Xbox Live",
    "XNET": "networking",
    "XVOICE": "voice chat",
}

RENDERWARE_RE = re.compile(rb"RenderWare(?:\x00|\s)*(?:Version|V)?\s*"
                           rb"([0-9]+\.[0-9]+(?:\.[0-9]+)*)", re.I)


class Survey:
    """Everything the XBE header can tell us about one title."""

    def __init__(self, path):
        self.path = Path(path)
        self.error = None

        self.title = ""
        self.title_id = 0
        self.region = 0
        self.is_debug = False

        self.libraries = []        # list of (name, major, minor, build)
        self.xdk_build = 0
        self.d3d_library = ""      # "D3D8", "D3D8LTCG", or "" if absent
        self.ltcg = False

        self.renderware = False
        self.renderware_version = ""

        self.text_size = 0
        self.image_size = 0
        self.num_sections = 0
        self.demand_loaded = []    # section names without the preload flag
        self.kernel_imports = 0
        self.notes = []

        try:
            self._parse()
        except Exception as exc:                       # noqa: BLE001
            self.error = f"{type(exc).__name__}: {exc}"

    def _parse(self):
        parser = XBEParser(str(self.path))
        xbe = parser.parse()
        raw = self.path.read_bytes()

        self.title = xbe.certificate.title_name
        self.title_id = xbe.certificate.title_id
        self.region = xbe.certificate.game_region
        self.is_debug = xbe.header.is_debug
        self.image_size = xbe.header.image_size
        self.kernel_imports = len(xbe.kernel_imports)

        for lib in xbe.libraries:
            name = lib.name.strip()
            self.libraries.append((name, lib.major, lib.minor, lib.build))

            # The XDK build number is shared across a disc's libraries; take
            # the highest seen rather than trusting any single entry.
            self.xdk_build = max(self.xdk_build, lib.build)

            upper = name.upper()
            if upper.startswith("D3D8"):
                # "D3D8LTCG" means link-time code generation. It makes signature
                # matching harder but is not disqualifying: the existing proven
                # target is itself an LTCG build.
                self.d3d_library = name
                self.ltcg = "LTCG" in upper
            if upper in COSTLY_LIBRARIES:
                self.notes.append(f"{name} ({COSTLY_LIBRARIES[upper]})")

        self.num_sections = len(xbe.sections)
        for sec in xbe.sections:
            if sec.name == ".text":
                self.text_size = sec.virtual_size
            if not sec.flags & SECTION_PRELOAD:
                self.demand_loaded.append(sec.name)
            bare = sec.name.lstrip(".").upper()
            if bare in COSTLY_SECTIONS:
                self.notes.append(f"{sec.name} section ({COSTLY_SECTIONS[bare]})")

        match = RENDERWARE_RE.search(raw)
        if match:
            self.renderware = True
            self.renderware_version = match.group(1).decode("ascii", "replace")
        elif b"RenderWare" in raw or b"RwEngine" in raw:
            self.renderware = True

    # -- ranking ---------------------------------------------------------

    def score(self):
        """Lower is a better first target.

        Weighted so that the things that actually stop a bring-up dominate:
        unimplemented subsystems first, then code volume.
        """
        if self.error:
            return float("inf")

        score = 0.0

        # Code volume is the main driver of lifting and debugging time.
        score += self.text_size / (256 * 1024)

        # A documented engine means someone else has already mapped the
        # structures you will be staring at in the debugger.
        if self.renderware:
            score -= 3.0

        # Each unimplemented subsystem is a bring-up blocker, not a detail.
        score += 5.0 * len(self.notes)

        # Demand-loaded sections must be paged in on demand at runtime; each is
        # a chance to fault somewhere unrelated to the code you are debugging.
        score += 0.5 * len(self.demand_loaded)

        # Debug XBEs use debug-era XDK patterns the toolkit sees less often.
        if self.is_debug:
            score += 2.0

        return score

    def row(self):
        return {
            "title": self.title,
            "title_id": f"0x{self.title_id:08X}",
            "path": str(self.path),
            "xdk_build": self.xdk_build,
            "d3d": self.d3d_library,
            "ltcg": "yes" if self.ltcg else "no",
            "renderware": self.renderware_version or ("yes" if self.renderware else ""),
            "text_kb": round(self.text_size / 1024, 1),
            "image_kb": round(self.image_size / 1024, 1),
            "kernel_imports": self.kernel_imports,
            "sections": self.num_sections,
            "demand_loaded": len(self.demand_loaded),
            "debug": "yes" if self.is_debug else "no",
            "concerns": "; ".join(self.notes),
            "score": round(self.score(), 2),
            "error": self.error or "",
        }


def find_xbes(root: Path):
    """Yield every XBE under root, or root itself if it is a file."""
    if root.is_file():
        return [root]

    found = sorted(root.rglob("*.xbe")) + sorted(root.rglob("*.XBE"))
    # rglob is case-sensitive on POSIX; de-duplicate for case-insensitive mounts.
    seen, unique = set(), []
    for p in found:
        key = str(p).lower()
        if key not in seen:
            seen.add(key)
            unique.append(p)
    return unique


def print_verbose(s: Survey):
    print(f"\n{'=' * 66}")
    print(f"  {s.title or '(no title)'}")
    print(f"  {s.path}")
    print("=" * 66)

    if s.error:
        print(f"  ERROR: {s.error}")
        return

    print(f"  Title ID        0x{s.title_id:08X}")
    print(f"  Build           {'debug' if s.is_debug else 'retail'}")
    print(f"  XDK build       {s.xdk_build or 'unknown'}")
    print(f"  Direct3D        {s.d3d_library or 'not linked'}"
          f"{'   (LTCG)' if s.ltcg else ''}")
    print(f"  RenderWare      {s.renderware_version or ('detected' if s.renderware else 'not detected')}")
    print(f"  .text           {s.text_size / 1024:.1f} KB")
    print(f"  Image           {s.image_size / 1024:.1f} KB")
    print(f"  Kernel imports  {s.kernel_imports}")
    print(f"  Sections        {s.num_sections} "
          f"({len(s.demand_loaded)} demand-loaded)")

    if s.libraries:
        print("\n  Libraries:")
        for name, major, minor, build in sorted(s.libraries):
            print(f"    {name:<16} {major}.{minor}.{build}")

    if s.demand_loaded:
        print("\n  Demand-loaded sections:")
        print("    " + ", ".join(s.demand_loaded))

    if s.notes:
        print("\n  Concerns:")
        for note in s.notes:
            print(f"    - {note}")

    print(f"\n  Score           {s.score():.2f}  (lower is a better first target)")


FIELDS = ["score", "title", "xdk_build", "d3d", "ltcg", "renderware", "text_kb",
          "kernel_imports", "demand_loaded", "debug", "concerns", "title_id",
          "image_kb", "sections", "path", "error"]


def main() -> int:
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("path", help="An XBE, or a directory of extracted discs")
    ap.add_argument("--csv", metavar="FILE", help="Write full results as CSV")
    ap.add_argument("--verbose", "-v", action="store_true",
                    help="Print a full report per title")
    ap.add_argument("--limit", type=int, metavar="N",
                    help="Show only the N best-ranked titles in the table")
    args = ap.parse_args()

    root = Path(args.path)
    if not root.exists():
        print(f"error: not found: {root}", file=sys.stderr)
        return 1

    paths = find_xbes(root)
    if not paths:
        print(f"error: no .xbe files under {root}", file=sys.stderr)
        return 1

    print(f"Surveying {len(paths)} XBE(s)...", file=sys.stderr)
    surveys = [Survey(p) for p in paths]
    surveys.sort(key=Survey.score)

    if args.verbose:
        for s in surveys:
            print_verbose(s)
    else:
        shown = surveys[:args.limit] if args.limit else surveys
        print()
        print(f"{'SCORE':>6}  {'TITLE':<30} {'XDK':>6} {'D3D':<9} "
              f"{'RW':<8} {'.text':>8} {'IMP':>4}  CONCERNS")
        print("-" * 100)
        for s in shown:
            if s.error:
                print(f"{'':>6}  {s.path.name:<30} ERROR: {s.error}")
                continue
            print(f"{s.score():>6.1f}  {(s.title or s.path.name)[:30]:<30} "
                  f"{s.xdk_build:>6} {s.d3d_library[:9]:<9} "
                  f"{(s.renderware_version or ('yes' if s.renderware else '-'))[:8]:<8} "
                  f"{s.text_size / 1024:>7.0f}K {s.kernel_imports:>4}  "
                  f"{'; '.join(s.notes)}")

    if args.csv:
        with open(args.csv, "w", newline="", encoding="utf-8") as f:
            writer = csv.DictWriter(f, fieldnames=FIELDS)
            writer.writeheader()
            for s in surveys:
                writer.writerow(s.row())
        print(f"\nWrote {args.csv}", file=sys.stderr)

    print("\nThe ranking is a starting point. It reads the XBE header only, so it",
          file=sys.stderr)
    print("cannot see hand-rolled push buffers, threading complexity, or whether",
          file=sys.stderr)
    print("you want to play the game. Bring up a second title on the same XDK as",
          file=sys.stderr)
    print("your proven one first: anything that breaks is something the project",
          file=sys.stderr)
    print("wrongly treated as universal.", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
