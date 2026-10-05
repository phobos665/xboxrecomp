"""
What recomp_manual.c defines.

"recomp_manual.c" here means the title's hand-written override sources: that
one file, plus every *.c under a folder given in its place (a title project's
src/overrides/, so overrides can be kept one subsystem to a file). Every
function below takes a path or a list of paths, and a directory stands for
the C files under it.

Single source of truth, deliberately. Both the recompiler (deciding what NOT to
generate) and gen_dangling_stubs.py (deciding what still needs a stub) have to
agree on what counts as a definition. When they each had their own regex they
disagreed, and every disagreement is a link error: a definition missed by one
becomes "already defined", a definition invented by the other becomes
"unresolved external".
"""

import os
import re
import sys

# A definition is "void sub_XXXXXXXX(void)" followed by a body -- on the same
# line (`void sub_X(void) { esp += 4; return; }`) or the next. A declaration
# ends in ';' and must not match.
DEF_RE = re.compile(r"^void (\w*?(?:sub_)?([0-9A-Fa-f]{8}))\(void\)\s*(?:\{|$)", re.M)
_SUB_DEF_RE = re.compile(r"^void (sub_([0-9A-Fa-f]{8}))\(void\)\s*(?:\{|$)", re.M)
# recomp_manual.c wraps rather than replaces some functions: it defines sub_X
# itself and calls the generated body as sub_X_gen.
_WRAP_RE = re.compile(r"^extern void (sub_([0-9A-Fa-f]{8}))_gen\(void\);", re.M)


def strip_disabled(src):
    """Remove '#if 0 ... #endif' regions.

    recomp_manual.c keeps disabled overrides around as documentation. A
    definition inside one is not a definition; counting it makes the
    recompiler skip a function that then nothing defines.
    """
    out, depth = [], 0
    for line in src.splitlines(keepends=True):
        s = line.strip()
        if re.match(r"^#if\s+0\b", s):
            depth += 1
        elif depth and re.match(r"^#if(def|ndef)?\b", s):
            depth += 1  # nested conditional inside a disabled block
        elif depth and s.startswith("#endif"):
            depth -= 1
        elif depth == 0:
            out.append(line)
    return "".join(out)


def manual_sources(paths):
    """The C files `paths` stands for, in a stable order.

    A file is itself; a directory is every *.c beneath it, sorted, so two
    machines scan the same files in the same order. A missing path is
    skipped (the caller decides whether that deserves a warning).
    """
    if isinstance(paths, (str, os.PathLike)):
        paths = [paths]
    out = []
    for p in paths:
        p = os.fspath(p)
        if os.path.isdir(p):
            for root, dirs, files in os.walk(p):
                dirs.sort()
                out.extend(os.path.join(root, f) for f in sorted(files)
                           if f.endswith(".c"))
        elif os.path.isfile(p):
            out.append(p)
    return out


def _live_sources(paths):
    """[(file, source with #if 0 regions removed)] for every manual source."""
    return [(f, strip_disabled(open(f, encoding="utf-8", errors="replace").read()))
            for f in manual_sources(paths)]


def duplicate_definitions(paths):
    """{name: [files]} for each sub_ defined in more than one manual source.

    One override per function: the second definition is a duplicate symbol at
    link time, and the message the linker gives names neither file usefully.
    """
    seen = {}
    for f, live in _live_sources(paths):
        for m in _SUB_DEF_RE.finditer(live):
            seen.setdefault(m.group(1), []).append(f)
    return {name: files for name, files in seen.items() if len(files) > 1}


def definition_names(paths):
    """Names of functions actually defined (compiled) in `paths`."""
    names = set()
    for _, live in _live_sources(paths):
        names |= {m.group(1) for m in _SUB_DEF_RE.finditer(live)}
    return names


_REF_RE = re.compile(r"\bsub_([0-9A-Fa-f]{8})\b")


def scan(path):
    """(skip, wrap, referenced) for functions the manual sources handle.

    `path` is a file, a directory of override files, or a list of either.

    skip:       defined by hand -- gen must declare but not define them.
    wrap:       manual calls the generated body as sub_X_gen -- gen must still
                emit it, renamed.
    referenced: every sub_XXXXXXXX the hand-written code mentions at all. Those
                must keep the sub_ name in the generated output: recomp_manual.c
                declares and calls sub_00350C10, so if a naming pass renamed it
                to SwapCopy_D3D_..., gen defines that and nothing defines
                sub_00350C10. Correctness beats a prettier name.
    """
    sources = _live_sources(path)
    if not sources:
        print(f"WARNING: {path} not found; excluding nothing.", file=sys.stderr)
        return set(), set(), set()
    for name, files in sorted(duplicate_definitions(path).items()):
        print(f"WARNING: {name} is defined in more than one override file "
              f"({', '.join(files)}); the link will fail on it.",
              file=sys.stderr)
    skip, wrap, referenced = set(), set(), set()
    for _, live in sources:
        skip |= {int(m.group(2), 16) for m in _SUB_DEF_RE.finditer(live)}
        wrap |= {int(m.group(2), 16) for m in _WRAP_RE.finditer(live)}
        referenced |= {int(m.group(1), 16) for m in _REF_RE.finditer(live)}
    return skip, wrap, referenced
