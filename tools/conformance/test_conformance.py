"""Runs the differential conformance suite as part of `pytest tools/`.

Natively it needs a 32-bit MSVC to assemble the snippets and build the
harness, so that half is Windows-only. Golden mode (--golden) checks the
lifted code against results recorded on Windows, and runs anywhere with a C
compiler -- including AArch64, where the lifted code is no longer checked by
the CPU it came from.
"""

import json
import math
import os
import struct
import tempfile
import unittest

from . import __main__ as conformance


class ConformanceTest(unittest.TestCase):
    def test_lifted_code_matches_the_cpu(self):
        if conformance._find_vcvars() is None:
            self.skipTest("needs a 32-bit MSVC (vcvars32.bat) to assemble and "
                          "build the harness")
        rc = conformance.main_with_args([])
        self.assertEqual(rc, 0, "the lifted C disagreed with the CPU; run "
                                "`py -3 -m tools.conformance` for the detail")

    def test_lifted_code_matches_the_recorded_cpu(self):
        if not os.path.exists(conformance.GOLDEN_DEFAULT):
            self.skipTest("no golden values recorded yet (record them on "
                          "Windows with --record)")
        if conformance._host_cc() is None:
            self.skipTest("no C compiler")
        rc = conformance.run_golden(conformance.GOLDEN_DEFAULT)
        self.assertEqual(rc, 0, "the lifted C disagreed with the recorded CPU; "
                                "run `python -m tools.conformance --golden`")


# ── golden mode itself, on values worked out from the Intel SDM ─────────────
#
# A recording made here, by hand, instead of by MSVC and an x86 CPU: the bytes
# are the instructions' encodings and the results are what the SDM says. That
# checks the golden machinery (load, compare, report) on any host, and the
# conversions in particular are the cases AArch64 gets wrong without help.

_ZERO_ST = "00" * 64
_ZERO_XMM = "00" * 128
_SCRATCH_VA = 0x00400000

_GPR_IN = [(0, 1), (1, 1), (0x7FFFFFFF, 1), (0x80000000, 0xFFFFFFFF),
           (0xFFFFFFFF, 0x7F), (12345678, 0x80), (0x12345678, 0xFFFF)]


def _gpr_case(code, fn):
    return {"kind": "gpr", "why": "synthetic", "tol": 0.0, "code": code,
            "inputs": [list(i) for i in _GPR_IN],
            "vectors": [[fn(a, b) & 0xFFFFFFFF, 0, _ZERO_ST, _ZERO_XMM]
                        for a, b in _GPR_IN]}


def _s32(v):
    return v - (1 << 32) if v & 0x80000000 else v


def _cvt(f, truncate):
    """cvt(t)ss2si: nearest-even or truncation, 0x80000000 when it cannot."""
    if math.isnan(f) or math.isinf(f):
        return 0x80000000
    r = math.trunc(f) if truncate else round(f)     # round() is half-to-even
    return r & 0xFFFFFFFF if -2**31 <= r < 2**31 else 0x80000000


_FLOATS = [2.5, 3.5, -2.5, 2.7, -2.7, 0.0, -0.0, 1e10, -3e9,
           2147483520.0, -2147483648.0, float("nan"), float("inf")]


def _sse_case(code, truncate):
    # Inputs are rounded through float32 first, as the scratch buffer holds them.
    vals = [struct.unpack("<f", struct.pack("<f", v))[0] for v in _FLOATS]
    return {"kind": "sse", "why": "synthetic", "tol": 0.0, "code": code,
            "inputs": [[[v, 0.0, 0.0, 0.0], [0.0, 0.0, 0.0, 0.0]] for v in vals],
            "vectors": [[_cvt(v, truncate), 0, _ZERO_ST, _ZERO_XMM] for v in vals]}


def _synthetic():
    def idiv(a, b):
        q = abs(_s32(a)) // abs(_s32(b))
        return q if (_s32(a) < 0) == (_s32(b) < 0) else -q
    return {"version": 1, "scratch_va": _SCRATCH_VA, "cases": {
        "syn_add": _gpr_case("01c8", lambda a, b: a + b),               # add eax, ecx
        "syn_sub": _gpr_case("29c8", lambda a, b: a - b),               # sub eax, ecx
        "syn_imul": _gpr_case("0fafc1", lambda a, b: _s32(a) * _s32(b)),  # imul eax, ecx
        "syn_idiv": _gpr_case("99f7f9", idiv),                          # cdq; idiv ecx
        "syn_cvtss2si": _sse_case("f30f2d00", False),                   # cvtss2si eax, [eax]
        "syn_cvttss2si": _sse_case("f30f2c00", True),                   # cvttss2si eax, [eax]
    }}


class GoldenModeTest(unittest.TestCase):
    def _run(self, gold):
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, "gold.json")
            with open(path, "w") as f:
                json.dump(gold, f)
            return conformance.run_golden(path, k="syn_")

    def setUp(self):
        if conformance._host_cc() is None:
            self.skipTest("no C compiler")

    def test_lifted_code_matches_sdm_results(self):
        self.assertEqual(self._run(_synthetic()), 0)

    def test_a_wrong_recording_is_reported(self):
        gold = _synthetic()
        gold["cases"]["syn_add"]["vectors"][2][0] ^= 1
        self.assertEqual(self._run(gold), 1)


if __name__ == "__main__":
    unittest.main()
