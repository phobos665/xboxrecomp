"""entry_pops_unsaved: does this address restore registers it never saved?

    python3 -m unittest tools.disasm.test_entry_pops

A seeded address that pops a callee-saved register it never pushed is the
middle of a function, not an entry point. Calling it runs an epilogue against
the caller's stack and hands the caller back a corrupted register, which
surfaces nowhere near the cause -- so the seeder has to reject it, and this
pins down exactly which shapes it rejects.
"""

import unittest

from .engine import DisasmEngine


class _Section:
    def __init__(self, va, size, executable=True):
        self.virtual_addr = va
        self.virtual_size = size
        self.executable = executable
        self.name = ".text"


class _Image:
    """Just enough of the image interface for the linear probe."""

    def __init__(self, va, code):
        self.va = va
        self.code = code
        self.sections = [_Section(va, len(code))]

    def get_section_at_va(self, addr):
        if self.va <= addr < self.va + len(self.code):
            return self.sections[0]
        return None

    def read_bytes_at_va(self, addr, length):
        if not (self.va <= addr < self.va + len(self.code)):
            return b""
        off = addr - self.va
        return self.code[off:off + length]


BASE = 0x00011000


def probe(code, offset=0):
    engine = DisasmEngine(_Image(BASE, code))
    return engine.entry_pops_unsaved(BASE + offset)


class ProperFunctions(unittest.TestCase):
    """A real entry point saves what it restores."""

    def test_push_pop_pairs_are_clean(self):
        # push esi ; push edi ; xor eax,eax ; pop edi ; pop esi ; ret
        code = bytes([0x56, 0x57, 0x31, 0xC0, 0x5F, 0x5E, 0xC3])
        self.assertEqual(probe(code), [])

    def test_one_push_answering_several_pops_is_clean(self):
        # A function with two return paths pops its single saved esi twice.
        # push esi ; xor eax,eax ; jz +2 ; pop esi ; ret ; pop esi ; ret
        code = bytes([0x56, 0x31, 0xC0, 0x74, 0x02, 0x5E, 0xC3, 0x5E, 0xC3])
        self.assertEqual(probe(code), [])

    def test_function_touching_no_saved_registers_is_clean(self):
        # mov eax,1 ; ret
        code = bytes([0xB8, 0x01, 0x00, 0x00, 0x00, 0xC3])
        self.assertEqual(probe(code), [])


class MidFunctionEntries(unittest.TestCase):
    """The shape the seeder has to reject."""

    def test_pop_without_push_is_reported(self):
        # pop esi ; ret   -- the tail of someone else's function
        code = bytes([0x5E, 0xC3])
        self.assertEqual(probe(code), ["esi"])

    def test_reports_every_offending_register(self):
        # pop edi ; pop esi ; pop ebx ; ret
        code = bytes([0x5F, 0x5E, 0x5B, 0xC3])
        self.assertEqual(sorted(probe(code)), ["ebx", "edi", "esi"])

    def test_entering_past_the_prologue_is_caught(self):
        # The whole function is clean, but entering after its pushes is not.
        # push esi ; push edi ; xor eax,eax ; pop edi ; pop esi ; ret
        code = bytes([0x56, 0x57, 0x31, 0xC0, 0x5F, 0x5E, 0xC3])
        self.assertEqual(probe(code), [], "the real entry is clean")
        # ...enter two bytes in, past both pushes:
        self.assertEqual(sorted(probe(code, offset=2)), ["edi", "esi"])

    def test_a_register_pushed_after_the_pop_does_not_excuse_it(self):
        # pop esi ; push esi ; ret -- the pop still ran against the caller.
        code = bytes([0x5E, 0x56, 0xC3])
        self.assertEqual(probe(code), ["esi"])


class Boundaries(unittest.TestCase):

    def test_scanning_stops_at_the_first_ret(self):
        # ret ; pop esi -- the pop belongs to whatever follows, not to us.
        code = bytes([0xC3, 0x5E])
        self.assertEqual(probe(code), [])

    def test_address_outside_the_image_is_not_reported(self):
        code = bytes([0x5E, 0xC3])
        engine = DisasmEngine(_Image(BASE, code))
        self.assertEqual(engine.entry_pops_unsaved(BASE + 0x9000), [])

    def test_eax_is_not_callee_saved(self):
        # pop eax ; ret -- caller-saved, so popping it proves nothing.
        code = bytes([0x58, 0xC3])
        self.assertEqual(probe(code), [])


if __name__ == "__main__":
    unittest.main()
