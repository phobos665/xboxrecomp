"""Loop headers carry RECOMP_BACKEDGE(), and nothing else does.

With the guest lock on, a guest thread spinning in lifted code waiting for
another guest thread would hold the lock forever; the back edge is where it
lets go (recomp_types.h). A loop header is a block that a later block, or the
block itself, jumps back to.
"""

import unittest

from . import config
from .translator import FunctionTranslator
from .test_translator_ebp_init import _CONFIG_GLOBALS

BASE = 0x00011000

# spin:  mov eax, [ecx]; test eax, eax; jz spin; ret
SPIN = bytes.fromhex("8B0185C074FAC3")
# Straight-line with a forward branch only:
#        test ecx, ecx; jz done; inc eax; done: ret
FORWARD = bytes.fromhex("85C9740140C3")
# A counted loop whose header is not the function start:
#        xor eax, eax; top: inc eax; dec ecx; jnz top; ret
COUNTED = bytes.fromhex("31C040497FFCC3")


class BackedgeTest(unittest.TestCase):
    def setUp(self):
        self._saved = {k: getattr(config, k) for k in _CONFIG_GLOBALS}

    def tearDown(self):
        for k, v in self._saved.items():
            setattr(config, k, v)

    def _translate(self, image):
        config._install(
            [config.Section(".text", BASE, len(image), 0x0000, len(image), True)],
            entry_point=BASE, kernel_thunk_addr=BASE, origin="backedge-test")
        db = {BASE: {"start": f"0x{BASE:08X}", "end": BASE + len(image),
                     "_addr": BASE, "size": len(image)}}
        return FunctionTranslator(image, db).translate_function(BASE, db[BASE])

    def test_a_spin_loop_checks_at_its_header(self):
        c = self._translate(SPIN)
        self.assertEqual(c.count("RECOMP_BACKEDGE();"), 1)
        header = c.index(f"loc_{BASE:08X}:")
        self.assertLess(header, c.index("RECOMP_BACKEDGE();"))
        self.assertLess(c.index("RECOMP_BACKEDGE();"), c.index("MEM32(ecx)"))

    def test_no_loop_no_check(self):
        self.assertNotIn("RECOMP_BACKEDGE", self._translate(FORWARD))

    def test_the_header_is_the_jump_target(self):
        c = self._translate(COUNTED)
        self.assertEqual(c.count("RECOMP_BACKEDGE();"), 1)
        top = c.index(f"loc_{BASE + 2:08X}:")
        self.assertLess(top, c.index("RECOMP_BACKEDGE();"))


if __name__ == "__main__":
    unittest.main()
