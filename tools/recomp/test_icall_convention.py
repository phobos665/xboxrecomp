"""Where the _icall_esp capture goes depends on who cleans the arguments.

    python3 -m unittest tools.recomp.test_icall_convention

RECOMP_ICALL_SAFE rewinds g_esp to the captured value when the lookup fails, so
the capture point decides what a failed indirect call does to the stack:

  stdcall  the callee would have cleaned the arguments, so the failure path has
           to clean them. Capture above the pushes.
  cdecl    the caller cleans them a few lines later whatever happens, so
           capturing above the pushes cleans them twice. Capture below.

Getting it wrong is silent: the caller's epilogue then pops its saved registers
from the wrong slots and returns corrupted values to *its* caller, faulting
somewhere with no connection to the call that failed.
"""

import unittest

from .translator import _fixup_icall_esp_save


def capture_index(lines):
    out = _fixup_icall_esp_save(lines)
    return out, next(i for i, l in enumerate(out) if "_icall_esp = g_esp" in l)


class CdeclSites(unittest.TestCase):
    """The caller cleans, so the failure path must not."""

    def test_capture_goes_below_the_arguments(self):
        lines = [
            "    PUSH32(esp, 0);",
            "    PUSH32(esp, 0x5ADDD0u);",
            "    PUSH32(esp, 0x5ADCB0u);",
            "    PUSH32(esp, 4);",
            "    PUSH32(esp, 0x000E8C44u); RECOMP_ICALL_SAFE(MEM32(esi + 4), _icall_esp); /* indirect call */",
            "loc_000E8C44: ;",
            "    esp = esp + 0x10;",
        ]
        out, save = capture_index(lines)
        icall = next(i for i, l in enumerate(out) if "RECOMP_ICALL_SAFE(" in l)
        first_arg = next(i for i, l in enumerate(out) if l.strip() == "PUSH32(esp, 0);")
        self.assertGreater(save, first_arg,
                           "capture must sit below the argument pushes:\n" + "\n".join(out))
        self.assertLess(save, icall)

    def test_decimal_cleanup_is_recognised(self):
        lines = [
            "    PUSH32(esp, eax);",
            "    PUSH32(esp, 0x00120010u); RECOMP_ICALL_SAFE(edx, _icall_esp); /* indirect call */",
            "    esp = esp + 4;",
        ]
        out, save = capture_index(lines)
        icall = next(i for i, l in enumerate(out) if "RECOMP_ICALL_SAFE(" in l)
        arg = next(i for i, l in enumerate(out) if l.strip() == "PUSH32(esp, eax);")
        self.assertGreater(save, arg)
        self.assertLess(save, icall)


class StdcallSites(unittest.TestCase):
    """No cleanup follows, so the callee owned the arguments."""

    def test_capture_stays_above_the_arguments(self):
        lines = [
            "    PUSH32(esp, ecx);",
            "    PUSH32(esp, edx);",
            "    PUSH32(esp, 0x00120010u); RECOMP_ICALL_SAFE(eax, _icall_esp); /* indirect call */",
            "    eax = MEM32(esi + 8);",
        ]
        out, save = capture_index(lines)
        first_arg = next(i for i, l in enumerate(out) if l.strip() == "PUSH32(esp, ecx);")
        self.assertLess(save, first_arg,
                        "capture must sit above the argument pushes:\n" + "\n".join(out))

    def test_an_unrelated_esp_write_is_not_a_cleanup(self):
        # "esp = esp - N" is a frame allocation, not an argument cleanup.
        lines = [
            "    PUSH32(esp, ecx);",
            "    PUSH32(esp, 0x00120010u); RECOMP_ICALL_SAFE(eax, _icall_esp); /* indirect call */",
            "    esp = esp - 8;",
        ]
        out, save = capture_index(lines)
        first_arg = next(i for i, l in enumerate(out) if l.strip() == "PUSH32(esp, ecx);")
        self.assertLess(save, first_arg)

    def test_a_following_call_ends_the_search(self):
        # The cleanup after a *later* call says nothing about this one.
        lines = [
            "    PUSH32(esp, ecx);",
            "    PUSH32(esp, 0x00120010u); RECOMP_ICALL_SAFE(eax, _icall_esp); /* indirect call */",
            "    PUSH32(esp, 0x00120020u); sub_00130000(); /* call 0x00130000 */",
            "    esp = esp + 4;",
        ]
        out, save = capture_index(lines)
        first_arg = next(i for i, l in enumerate(out) if l.strip() == "PUSH32(esp, ecx);")
        self.assertLess(save, first_arg)


class StillHonoursTheOlderRules(unittest.TestCase):
    """The convention check must not undo what the argument-run scan learned."""

    def test_a_callee_saved_push_still_ends_the_run(self):
        lines = [
            "    PUSH32(esp, esi);",
            "    PUSH32(esp, ecx);",
            "    PUSH32(esp, 0x00120010u); RECOMP_ICALL_SAFE(eax, _icall_esp); /* indirect call */",
            "    POP32(esp, esi);",
        ]
        out, save = capture_index(lines)
        esi_push = next(i for i, l in enumerate(out) if l.strip() == "PUSH32(esp, esi);")
        self.assertGreater(save, esi_push,
                           "the esi save belongs to the frame, not the call:\n" + "\n".join(out))


if __name__ == "__main__":
    unittest.main()
