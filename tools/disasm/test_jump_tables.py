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
