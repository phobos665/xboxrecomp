"""Opt-in code generation changes for speed.

Each of these changes how a guest instruction is spelled in C, never what it
does. They are off unless asked for, so a lift without them is byte-for-byte
what it was before they existed, and a title only takes one after it has been
run with it. docs/technical/performance-upgrades-testing.md is the procedure;
docs/technical/lifted-code-quality-review.md is the measurement behind each.

    py -3 -m tools.recomp ... --perf-opts all
    py -3 scripts/recompile.py ... --perf-opts stosd,leaf-cache
    XBOXRECOMP_PERF_OPTS=all py -3 scripts/build.py --relift    # any title

The environment variable exists so a title's own build script passes it
through without knowing about it. An explicit --perf-opts wins over it.
"""

import os

ENV_VAR = "XBOXRECOMP_PERF_OPTS"

# name -> one line for --help and the lift log
OPTS = {
    "stosd": "rep stosd/stosw: registers read once, and memset for a clear",
    "rmw-snapshot": "a memory destination's flags come from the value "
                    "written, not a second read of guest memory",
    "fcmp-float": "comiss/ucomiss snapshots kept as float, not widened to "
                  "double (MSVC converts both operands at every compare)",
    "xmm-intrinsics": "packed SSE helpers on host SSE intrinsics; one 16-byte "
                      "access per movaps instead of four volatile dwords",
    "leaf-cache": "functions that call nothing keep guest registers in C "
                  "locals, written back at every exit",
}


def parse(text):
    """'all', 'none', '' or a comma list -> frozenset of option names."""
    text = (text or "").strip().lower()
    if text in ("", "none", "off", "0"):
        return frozenset()
    if text in ("all", "on", "1"):
        return frozenset(OPTS)
    names = {n.strip() for n in text.split(",") if n.strip()}
    unknown = names - set(OPTS)
    if unknown:
        raise ValueError(
            "unknown --perf-opts %s (known: %s, or all/none)"
            % (", ".join(sorted(unknown)), ", ".join(OPTS)))
    return frozenset(names)


def from_args(cli_value):
    """The options in force: --perf-opts if given, else the environment."""
    if cli_value is not None:
        return parse(cli_value)
    return parse(os.environ.get(ENV_VAR, ""))


def describe(opts):
    return ", ".join(n for n in OPTS if n in opts) if opts else "none"
