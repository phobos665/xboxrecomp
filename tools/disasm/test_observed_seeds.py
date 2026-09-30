"""Seeds a run actually reached survive the mid-instruction guard.

The guard rejects a seed that lands inside an instruction the linear sweep
already decoded, unless the seed decodes as a prologue. It was written for
RTTI vtable slots, which are inferences (HL2 had two bad ones in 12,288). A
seed from tools.seed_from_log is where the CPU actually went, so it is kept
even without a prologue: Halo's XPP init at 0x001CF6AC opens `cmp [flag], 0`
right after a pointer table the sweep had walked as code.
"""
import json
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

from tools.disasm.__main__ import _load_seed_functions  # noqa: E402
from tools.disasm.disasm import Disassembler  # noqa: E402


class _Engine:
    def __init__(self, prologues, after_padding=()):
        self._prologues = set(prologues)
        self._after_padding = set(after_padding)

    def probes_as_prologue(self, addr):
        return addr in self._prologues

    def follows_padding(self, addr):
        return addr in self._after_padding


def _disasm(observed, prologues, after_padding=()):
    d = Disassembler.__new__(Disassembler)
    d.observed_seeds = set(observed)
    d.engine = _Engine(prologues, after_padding)
    return d


class ObservedSeedTest(unittest.TestCase):
    def test_observed_seed_is_trusted_without_a_prologue(self):
        d = _disasm(observed={0x001CF6AC}, prologues=())
        self.assertTrue(d._trust_mid_instruction_seed(0x001CF6AC))

    def test_unobserved_non_prologue_seed_is_still_rejected(self):
        # The HL2 RTTI case: six bytes into a mov, decodes as nothing.
        d = _disasm(observed=(), prologues=())
        self.assertFalse(d._trust_mid_instruction_seed(0x00202C2E))

    def test_prologue_seed_is_trusted(self):
        d = _disasm(observed=(), prologues={0x00069538})
        self.assertTrue(d._trust_mid_instruction_seed(0x00069538))

    def test_seed_after_padding_is_trusted(self):
        # This fork's own exception: a function start right after the
        # linker's 0xCC / 0x90 padding, prologue or not.
        d = _disasm(observed=(), prologues=(), after_padding={0x00031000})
        self.assertTrue(d._trust_mid_instruction_seed(0x00031000))

    def test_loader_marks_observed_seeds(self):
        seeds = [
            {"start": "0x001CF6AC", "observed": True, "note": "x"},
            # Written by seed_from_log before the field existed.
            {"start": "0x00015C5A", "note": "PsCreateSystemThreadEx "
             "StartContext1 observed at runtime; decodes as a function body."},
            {"start": "0x00202C2E", "note": "RTTI vtable slot"},
            0x00010000,
        ]
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, "seeds.json")
            with open(path, "w") as f:
                json.dump(seeds, f)
            observed = set()
            addrs = _load_seed_functions(path, observed)
        self.assertEqual(sorted(addrs),
                         [0x00010000, 0x00015C5A, 0x001CF6AC, 0x00202C2E])
        self.assertEqual(observed, {0x001CF6AC, 0x00015C5A})


if __name__ == "__main__":
    unittest.main()
