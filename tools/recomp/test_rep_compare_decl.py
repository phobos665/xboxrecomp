"""A repe/repne cmps or scas declares the `_flags` its loop writes.

The declaration was keyed on the function having a jcc, setcc or cmov. The
inline strlen idiom -- `or ecx, -1; repne scasb; not ecx; dec ecx` -- has
none, so the lifted function assigned an undeclared `_flags` and did not
compile. Found by tools.codegen_ab, which compiles every conformance case.
"""

import unittest

from .test_perf_opts import _Lift


class RepCompareFlagsDeclarationTest(_Lift):
    def test_rep_compare_without_a_branch_declares_flags(self):
        c = self.lift("F2AFC3")                        # repne scasd; ret
        self.assertIn("_flags = (eax == MEM32(edi));", c)
        self.assertIn("int _flags = 0;", c)

    def test_strlen_idiom(self):
        # or ecx, -1; xor eax, eax; repne scasb; not ecx; dec ecx; ret
        c = self.lift("83C9FF33C0F2AEF7D149C3")
        self.assertIn("int _flags = 0;", c)


if __name__ == "__main__":
    unittest.main()
