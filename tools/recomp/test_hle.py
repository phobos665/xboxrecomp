"""
Self-check for tools.recomp.hle (replacing XDK functions by name).

Synthetic symbols, synthetic x86 and a temporary C file; no game files needed.
Run: py -3 -m unittest tools.recomp.test_hle
"""

import os
import tempfile
import unittest

from tools.recomp.hle import implemented_names, plan, render_thunks, stack_cleanup


def sym(name, addr):
    return {"name": name, "address": addr, "kind": "function"}


def pops_zero(addr):
    return 0


class ImplementedNames(unittest.TestCase):
    def test_markers_are_found_and_comments_are_not(self):
        with tempfile.TemporaryDirectory() as d:
            with open(os.path.join(d, "d3d.c"), "w") as f:
                f.write("HLE_EXPORT(D3DDevice_SetTexture)\n{\n}\n"
                        "  HLE_EXPORT( D3DDevice_Swap )\n{\n}\n"
                        "/* HLE_EXPORT(NotReal) is mentioned mid-comment */\n")
            self.assertEqual(implemented_names([d]),
                             {"D3DDevice_SetTexture", "D3DDevice_Swap"})


class StackCleanup(unittest.TestCase):
    def test_ret_with_a_count(self):
        # xor eax, eax ; ret 8
        self.assertEqual(stack_cleanup(bytes.fromhex("33c0c20800"), 0x1000), 8)

    def test_plain_ret_pops_no_arguments(self):
        self.assertEqual(stack_cleanup(bytes.fromhex("c3"), 0x1000), 0)

    def test_every_ret_must_agree(self):
        # ret 4 ; ret 8 -- no single answer
        self.assertIsNone(stack_cleanup(bytes.fromhex("c20400c20800"), 0x1000))

    def test_a_body_that_only_tail_jumps_has_no_answer(self):
        # jmp rel32
        self.assertIsNone(stack_cleanup(bytes.fromhex("e900000000"), 0x1000))


class Plan(unittest.TestCase):
    def test_an_implemented_detected_function_is_replaced(self):
        replace, notes = plan([sym("D3DDevice_Swap", 0x1000)],
                              {"D3DDevice_Swap"}, {0x1000}, set(), lambda a: 4)
        self.assertEqual(replace, {0x1000: ("D3DDevice_Swap", 4)})
        self.assertEqual(notes, [])

    def test_hand_written_title_code_wins(self):
        replace, notes = plan([sym("D3DDevice_Swap", 0x1000)],
                              {"D3DDevice_Swap"}, {0x1000}, {0x1000}, pops_zero)
        self.assertEqual(replace, {})
        self.assertIn("hand-written", notes[0])

    def test_a_name_matched_twice_is_not_guessed(self):
        replace, notes = plan([sym("D3DDevice_Swap", 0x1000), sym("D3DDevice_Swap", 0x2000)],
                              {"D3DDevice_Swap"}, {0x1000, 0x2000}, set(), pops_zero)
        self.assertEqual(replace, {})
        self.assertIn("2 matches", notes[0])

    def test_missing_and_undetected_are_reported(self):
        replace, notes = plan([sym("D3DDevice_Clear", 0x3000)],
                              {"D3DDevice_Clear", "D3DDevice_Swap"}, set(), set(), pops_zero)
        self.assertEqual(replace, {})
        self.assertEqual(len(notes), 2)

    def test_no_single_ret_is_skipped_rather_than_guessed(self):
        replace, notes = plan([sym("D3DDevice_Swap", 0x1000)],
                              {"D3DDevice_Swap"}, {0x1000}, set(), lambda a: None)
        self.assertEqual(replace, {})
        self.assertIn("no single", notes[0])

    def test_unimplemented_symbols_are_left_alone(self):
        replace, _ = plan([sym("D3DDevice_Clear", 0x3000)], set(), {0x3000}, set(), pops_zero)
        self.assertEqual(replace, {})


class Render(unittest.TestCase):
    def test_thunk_calls_the_implementation_then_pops_like_the_original(self):
        src = render_thunks({0x002158D0: ("D3DDevice_SetFlickerFilter", 4)})
        self.assertIn("void hle_D3DDevice_SetFlickerFilter(void);", src)
        self.assertIn("void sub_002158D0(void) { hle_D3DDevice_SetFlickerFilter(); g_esp += 8; }",
                      src)


if __name__ == "__main__":
    unittest.main()
