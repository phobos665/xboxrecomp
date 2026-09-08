"""x87 lifting tests, using synthetic instructions.

    python3 -m unittest tools.recomp.test_lifter_fpu

These need no game files, no compiler and no generated program: they build
Instruction/Operand objects directly and assert on the C the lifter emits.

x87 is where lifter bugs hide, because a wrong destination or a missing pop
produces no crash and no diagnostic — just numbers that drift from the
reference. Every case here is one that previously emitted silently wrong code.
"""

import unittest

from .disasm import Instruction, Operand
from .lifter import Lifter


def reg(name):
    return Operand(type="reg", reg=name)


def mem(size, base="ebx"):
    return Operand(type="mem", mem_base=base, mem_size=size)


def lift(mnemonic, op_str="", operands=None):
    insn = Instruction(0, 2, mnemonic, op_str, "")
    if operands:
        insn.operands = list(operands)
    return Lifter().lift_instruction(insn)[0]


class MemoryOperandsDoNotPop(unittest.TestCase):
    """`fadd dword ptr [ebx]` adds into ST(0) and leaves the stack depth alone.

    This is the most common x87 form in game code. Emitting a pop for it
    unbalances the stack for the whole rest of the function.
    """

    def test_add_float(self):
        out = lift("fadd", "dword ptr [ebx]", [mem(4)])
        self.assertIn("fp_top() = fp_top() + MEMF(ebx)", out)
        self.assertNotIn("fp_pop", out)

    def test_sub_double(self):
        out = lift("fsub", "qword ptr [ebx]", [mem(8)])
        self.assertIn("fp_top() = fp_top() - MEMD(ebx)", out)
        self.assertNotIn("fp_pop", out)

    def test_reverse_subtract_reverses_operands(self):
        # FSUBR m: ST(0) = m - ST(0), not ST(0) - m.
        out = lift("fsubr", "dword ptr [ebx]", [mem(4)])
        self.assertIn("fp_top() = MEMF(ebx) - fp_top()", out)

    def test_reverse_divide_reverses_operands(self):
        out = lift("fdivr", "dword ptr [ebx]", [mem(4)])
        self.assertIn("fp_top() = MEMF(ebx) / fp_top()", out)

    def test_integer_source_uses_signed_accessor(self):
        # FIADD takes an integer operand, not a float one.
        out = lift("fiadd", "dword ptr [ebx]", [mem(4)])
        self.assertIn("SMEM32(ebx)", out)


class RegisterDestinations(unittest.TestCase):
    """The first operand is the destination, and it is not always ST(1)."""

    def test_pop_form_honours_explicit_destination(self):
        out = lift("fsubp", "st(2), st(0)", [reg("st(2)"), reg("st(0)")])
        self.assertIn("fp_st(2) = fp_st(2) - fp_top();", out)
        self.assertIn("fp_pop();", out)

    def test_pop_form_defaults_to_st1(self):
        out = lift("fsubp", "", [])
        self.assertIn("fp_st1() = fp_st1() - fp_top();", out)
        self.assertIn("fp_pop();", out)

    def test_single_register_pop_form_is_the_destination(self):
        out = lift("fsubp", "st(3)", [reg("st(3)")])
        self.assertIn("fp_st(3) = fp_st(3) - fp_top();", out)

    def test_destination_st0(self):
        out = lift("fmul", "st(0), st(3)", [reg("st(0)"), reg("st(3)")])
        self.assertIn("fp_top() = fp_top() * fp_st(3);", out)
        self.assertNotIn("fp_pop", out)

    def test_destination_sti_no_pop(self):
        out = lift("fadd", "st(3), st(0)", [reg("st(3)"), reg("st(0)")])
        self.assertIn("fp_st(3) = fp_st(3) + fp_top();", out)
        self.assertNotIn("fp_pop", out)

    def test_reverse_pop_form(self):
        # FSUBRP st(1), st(0): ST(1) = ST(0) - ST(1), then pop.
        out = lift("fsubrp", "st(1), st(0)", [reg("st(1)"), reg("st(0)")])
        self.assertIn("fp_st1() = fp_top() - fp_st1();", out)
        self.assertIn("fp_pop();", out)

    def test_divrp_is_not_silently_dropped(self):
        out = lift("fdivrp", "st(1), st(0)", [reg("st(1)"), reg("st(0)")])
        self.assertNotIn("/* FPU:", out)
        self.assertIn("fp_st1() = fp_top() / fp_st1();", out)


class StackMovement(unittest.TestCase):

    def test_fxch_honours_operand(self):
        out = lift("fxch", "st(3)", [reg("st(3)")])
        self.assertIn("fp_st(3)", out)

    def test_fxch_defaults_to_st1(self):
        out = lift("fxch", "", [])
        self.assertIn("fp_st1()", out)

    def test_fld_sti_duplicates_before_pushing(self):
        # The push moves the stack, so the source must be read first.
        out = lift("fld", "st(2)", [reg("st(2)")])
        self.assertIn("double _t = fp_st(2);", out)
        self.assertIn("fp_push(_t);", out)

    def test_fistp_pops(self):
        out = lift("fistp", "dword ptr [ebx]", [mem(4)])
        self.assertIn("fp_pop();", out)

    def test_fist_does_not_pop(self):
        out = lift("fist", "dword ptr [ebx]", [mem(4)])
        self.assertNotIn("fp_pop", out)

    def test_fist_rounds_to_nearest(self):
        # The default control word rounds to nearest-even; truncating is an
        # off-by-one on half of all inputs.
        out = lift("fistp", "dword ptr [ebx]", [mem(4)])
        self.assertIn("nearbyint(fp_top())", out)

    def test_fisttp_truncates(self):
        out = lift("fisttp", "dword ptr [ebx]", [mem(4)])
        self.assertNotIn("nearbyint", out)
        self.assertIn("fp_pop();", out)

    def test_fistp_qword_uses_64_bit_type(self):
        out = lift("fistp", "qword ptr [ebx]", [mem(8)])
        self.assertIn("int64_t", out)


class Comparisons(unittest.TestCase):

    def test_fcom_uses_operand(self):
        out = lift("fcom", "st(3)", [reg("st(3)")])
        self.assertIn("fp_st(3)", out)

    def test_fcomp_pops_once(self):
        out = lift("fcomp", "st(1)", [reg("st(1)")])
        self.assertEqual(out.count("fp_pop();"), 1)

    def test_fcompp_pops_twice(self):
        out = lift("fcompp", "", [])
        self.assertEqual(out.count("fp_pop();"), 2)

    def test_fcom_does_not_pop(self):
        out = lift("fcom", "", [])
        self.assertNotIn("fp_pop", out)


class NoSilentDrops(unittest.TestCase):
    """Nothing in the common x87 set may fall through to a bare comment."""

    MNEMONICS = [
        ("fadd", [mem(4)]), ("faddp", [reg("st(1)"), reg("st(0)")]),
        ("fsub", [mem(4)]), ("fsubp", [reg("st(1)"), reg("st(0)")]),
        ("fsubr", [mem(4)]), ("fsubrp", [reg("st(1)"), reg("st(0)")]),
        ("fmul", [mem(4)]), ("fmulp", [reg("st(1)"), reg("st(0)")]),
        ("fdiv", [mem(4)]), ("fdivp", [reg("st(1)"), reg("st(0)")]),
        ("fdivr", [mem(4)]), ("fdivrp", [reg("st(1)"), reg("st(0)")]),
        ("fiadd", [mem(4)]), ("fisub", [mem(4)]),
        ("fimul", [mem(4)]), ("fidiv", [mem(4)]),
        ("fisubr", [mem(4)]), ("fidivr", [mem(4)]),
        ("fld", [mem(4)]), ("fld", [reg("st(1)")]),
        ("fild", [mem(4)]), ("fistp", [mem(4)]),
        ("fxch", [reg("st(1)")]),
    ]

    def test_no_mnemonic_emits_only_a_comment(self):
        for mnemonic, operands in self.MNEMONICS:
            with self.subTest(mnemonic=mnemonic):
                out = lift(mnemonic, "", operands)
                self.assertNotIn("/* FPU:", out,
                                 f"{mnemonic} lifted to a bare comment")


if __name__ == "__main__":
    unittest.main()
