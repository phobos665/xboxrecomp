"""shl/shr mask their count to five bits, as x86 does.

`shl byte ptr [esi], cl` with cl = 65 lifted to `MEM8(esi) << LO8(ecx)`: a
shift of 65, which C leaves undefined. It looked right while the count was a
run-time value (the compiler emits the hardware's own masking shift) and came
out differently once a constant count was in reach of the optimiser -- which
caching registers in locals provides. tools.codegen_ab found it.
"""

import unittest

from .test_perf_opts import _Lift


class ShiftCountMaskTest(_Lift):
    def test_register_count_is_masked(self):
        c = self.lift("D3E0C3")                        # shl eax, cl; ret
        self.assertIn("eax = eax << ((LO8(ecx)) & 31u);", c)
        c = self.lift("D3E8C3")                        # shr eax, cl
        self.assertIn("eax = eax >> ((LO8(ecx)) & 31u);", c)

    def test_immediate_spelling_is_unchanged(self):
        c = self.lift("C1E003C3")                      # shl eax, 3
        self.assertIn("eax = eax << 3;", c)

    def test_narrow_carry_never_shifts_by_a_negative_amount(self):
        # shl al, cl; adc edx, 0 -- the adc makes the function track CF.
        c = self.lift("D2E083D200C3")
        self.assertIn("_cf = (((LO8(ecx)) & 31u)) <= 8 ?", c)

    def test_wide_carry_is_as_before_but_masked(self):
        c = self.lift("D3E083D200C3")                  # shl eax, cl; adc edx, 0
        self.assertIn("if (((LO8(ecx)) & 31u)) _cf = (int)(((eax) >> (32 - "
                      "(((LO8(ecx)) & 31u)))) & 1);", c)


if __name__ == "__main__":
    unittest.main()
