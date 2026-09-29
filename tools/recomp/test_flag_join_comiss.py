"""SSE float compares from different predecessors merge at a join.

`comiss` snapshots both operands into _fca/_fcb, and every condition after
it reads only the snapshot, so two predecessors that compared different
operands leave a join in the same state: whichever ran wrote the values the
jcc reads. `_merge_flag_states` only merged cmp/test that way, so a jcc that
two comiss paths reach compiled as the `_flags` fallback and was never taken.

Dino Crisis 3 does its float maths in SSE. Its lifted code had ~100 such
branches -- `_fca = xmm0.f[0]; _fcb = MEMF(ecx + 0xE44); /* comiss */`, then
a label, then `if (_flags /* jbe */)` -- and the player walked through walls.
"""

import unittest

from .disasm import Operand
from .translator import _merge_flag_states


def _xmm(name):
    return Operand(type="reg", reg=name, mem_size=16)


def _mem(base, disp):
    return Operand(type="mem", mem_base=base, mem_disp=disp, mem_size=4)


class ComissJoinTest(unittest.TestCase):
    def test_two_comiss_with_different_operands_merge(self):
        a = ("comiss", [_xmm("xmm0"), _mem("ecx", 0xE44)])
        b = ("comiss", [_xmm("xmm1"), _xmm("xmm2")])
        merged = _merge_flag_states([a, b])
        self.assertIsNotNone(merged)
        self.assertEqual(merged[0], "comiss")

    def test_the_family_merges_across_its_members(self):
        """ucomiss and comiss set the same flags; comisd uses the same snapshot."""
        states = [("comiss", [_xmm("xmm0"), _xmm("xmm1")]),
                  ("ucomiss", [_xmm("xmm3"), _mem("esi", 0)]),
                  ("comisd", [_xmm("xmm4"), _xmm("xmm5")])]
        self.assertIsNotNone(_merge_flag_states(states))

    def test_a_comiss_does_not_merge_with_an_integer_compare(self):
        """cmp snapshots _fa/_fb, not _fca/_fcb: different state, no merge."""
        cmp = ("cmp", [Operand(type="reg", reg="eax", mem_size=4),
                       Operand(type="imm", imm=0, mem_size=4)])
        self.assertIsNone(_merge_flag_states(
            [("comiss", [_xmm("xmm0"), _xmm("xmm1")]), cmp]))
        self.assertIsNone(_merge_flag_states(
            [cmp, ("comiss", [_xmm("xmm0"), _xmm("xmm1")])]))


if __name__ == "__main__":
    unittest.main()
