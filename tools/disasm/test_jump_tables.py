"""resync_jump_tables: the index into a switch table can be negative.

    python3 -m unittest tools.disasm.test_jump_tables

`jmp [reg*4 + disp]` says where index 0 lands, not where the table begins.
MSVC's memcpy tail dispatch counts remaining bytes *down*, so disp names the
last entry and the rest of the table sits below it. Scanning forward from disp
finds one entry, rejects the table as too short, and leaves the sweep's
hallucinated instructions in place -- which ends the enclosing function inside
its own table, stranding the epilogue.
"""

import unittest
import struct

from .engine import DisasmEngine


class _Section:
    def __init__(self, va, size, executable=True):
        self.virtual_addr = va
        self.virtual_size = size
        self.executable = executable
        self.name = ".text"


class _Image:
    def __init__(self, va, code):
        self.va = va
        self.code = bytearray(code)
        self.base_address = va
        self.image_size = len(code)
        self.sections = [_Section(va, len(code))]

    def get_section_at_va(self, addr):
        if self.va <= addr < self.va + len(self.code):
            return self.sections[0]
        return None

    def read_bytes_at_va(self, addr, length):
        if not (self.va <= addr < self.va + len(self.code)):
            return b""
        off = addr - self.va
        return bytes(self.code[off:off + length])

    def get_section_data(self, section):
        return bytes(self.code)

    def read_u32_at_va(self, addr):
        raw = self.read_bytes_at_va(addr, 4)
        if len(raw) < 4:
            return None
        return struct.unpack("<I", raw)[0]


BASE = 0x00011000


def build(table_entries, disp_index):
    """A section holding a table of `table_entries` plus somewhere to point.

    The dispatch's displacement names entry `disp_index`, so a table addressed
    with a negative index has disp_index at the end.
    """
    size = 0x400
    img = _Image(BASE, bytes(size))

    table_va = BASE + 0x100
    for i, target in enumerate(table_entries):
        off = (table_va - BASE) + i * 4
        img.code[off:off + 4] = struct.pack("<I", target)

    disp = table_va + disp_index * 4
    # jmp dword ptr [ecx*4 + disp]  ->  ff 24 8d <disp>
    jmp_off = 0x20
    img.code[jmp_off:jmp_off + 3] = b"\xff\x24\x8d"
    img.code[jmp_off + 3:jmp_off + 7] = struct.pack("<I", disp)
    return img, table_va, BASE + jmp_off


class BackwardIndexedTables(unittest.TestCase):

    def test_a_table_addressed_from_its_last_entry_is_found(self):
        targets = [BASE + 0x200 + i * 8 for i in range(8)]
        img, table_va, jmp_va = build(targets, disp_index=len(targets) - 1)
        engine = DisasmEngine(img)
        engine.linear_sweep(img.sections[0])
        engine.resync_jump_tables()

        self.assertIn(table_va, engine.jump_tables,
                      "the table starts below the displacement, not at it")
        self.assertEqual(engine.jump_tables[table_va],
                         table_va + len(targets) * 4)
        self.assertEqual(engine.jump_table_entries(table_va), targets)

    def test_a_forward_table_still_works(self):
        targets = [BASE + 0x200 + i * 8 for i in range(6)]
        img, table_va, _ = build(targets, disp_index=0)
        engine = DisasmEngine(img)
        engine.linear_sweep(img.sections[0])
        engine.resync_jump_tables()

        self.assertIn(table_va, engine.jump_tables)
        self.assertEqual(engine.jump_table_entries(table_va), targets)


def build_inline(table_entries, disp_index):
    """The same, but with the table immediately after the dispatch.

    This is where MSVC actually puts it, and the distance is what matters:
    `build()` above leaves 0xE0 bytes between the jump and the table, so the
    backward scan runs out of plausible pointers long before it reaches the
    jump. Inline, the four bytes below the table *are* the jump's
    displacement, and its value is the table address -- a perfectly good code
    address for the scan to accept.
    """
    size = 0x400
    img = _Image(BASE, bytes(size))

    jmp_off = 0x20
    table_va = BASE + jmp_off + 7          # straight after ff 24 8d <disp32>
    for i, target in enumerate(table_entries):
        off = (table_va - BASE) + i * 4
        img.code[off:off + 4] = struct.pack("<I", target)

    disp = table_va + disp_index * 4
    img.code[jmp_off:jmp_off + 3] = b"\xff\x24\x8d"
    img.code[jmp_off + 3:jmp_off + 7] = struct.pack("<I", disp)
    for target in table_entries:
        img.code[target - BASE] = 0xC3     # ret, so the arms decode
    return img, table_va, BASE + jmp_off


class InlineTables(unittest.TestCase):
    """A table placed immediately after its dispatch, which is the usual case.

    Both of these failed before the backward scan learned to stop at the
    instruction that owns the bytes: the scan read the jump's own displacement
    as entry -1, moved the table start back a slot, and the cleanup loop then
    deleted the jump -- so _find_function_end saw neither a table nor an
    instruction at the dispatch and ended the function there. Every function
    with an inline switch was truncated, on every title.
    """

    def test_a_table_directly_after_its_dispatch_is_not_extended_backwards(self):
        targets = [BASE + 0x200 + i * 0x10 for i in range(4)]
        img, table_va, jmp_va = build_inline(targets, disp_index=0)
        engine = DisasmEngine(img)
        engine.linear_sweep(img.sections[0])
        engine.resync_jump_tables()

        self.assertIn(table_va, engine.jump_tables,
                      "the table starts where the jump ends, not four bytes below")
        self.assertEqual(engine.jump_tables[table_va], table_va + len(targets) * 4)
        self.assertEqual(engine.jump_table_entries(table_va), targets)
        self.assertIsNotNone(engine.instructions.get(jmp_va),
                             "the dispatch jump must survive the resync")

    def test_an_inline_table_addressed_from_its_last_entry_still_resolves(self):
        # The negative-index case the backward scan exists for, in the layout
        # it will actually meet: scanning down from the displacement has to
        # reach the table start and stop exactly at the end of the jump.
        targets = [BASE + 0x200 + i * 0x10 for i in range(4)]
        img, table_va, jmp_va = build_inline(targets, disp_index=len(targets) - 1)
        engine = DisasmEngine(img)
        engine.linear_sweep(img.sections[0])
        engine.resync_jump_tables()

        self.assertIn(table_va, engine.jump_tables)
        self.assertEqual(engine.jump_table_entries(table_va), targets)
        self.assertIsNotNone(engine.instructions.get(jmp_va),
                             "the dispatch jump must survive the resync")


class StillRejectsNonTables(unittest.TestCase):
    """Scanning both ways must not make a coincidence into a table."""

    def test_too_few_entries_either_way(self):
        # Two plausible pointers is under min_entries however you count them.
        targets = [BASE + 0x200, BASE + 0x208]
        img, table_va, _ = build(targets, disp_index=1)
        engine = DisasmEngine(img)
        engine.linear_sweep(img.sections[0])
        engine.resync_jump_tables()
        self.assertNotIn(table_va, engine.jump_tables)

    def test_entries_outside_the_section_bound_the_scan(self):
        # A run of three valid entries, then one pointing out of the section:
        # the table ends there rather than swallowing the rest.
        targets = [BASE + 0x200, BASE + 0x208, BASE + 0x210]
        img, table_va, _ = build(targets, disp_index=0)
        off = (table_va - BASE) + 3 * 4
        img.code[off:off + 4] = struct.pack("<I", 0xDEADBEEF)
        engine = DisasmEngine(img)
        engine.linear_sweep(img.sections[0])
        engine.resync_jump_tables()
        self.assertEqual(engine.jump_tables.get(table_va),
                         table_va + 3 * 4)


if __name__ == "__main__":
    unittest.main()
