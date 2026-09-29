"""
Self-check: restarting the decode at a switch-table target evicts the whole
out-of-phase run, not only the instruction straddling the target.

Run: py -3 tools/recomp/test_jump_table_resync_overlap.py

The bytes are Tenchu: Return from Darkness's memcpy tail, 0x0030C87A-0x0030C893:
`mov edi, edi`, the 4-entry trailing-byte table, and the 0-byte arm
`mov eax, [ebp+8]; pop esi; pop edi; leave; ret`. A linear decode of the table
comes out of it one byte past the arm's start, as `inc ebp` at 0x0030C88D and
`or [esi+0x5F], bl` at 0x0030C88E. Only the straddler was dropped, so those two
were lifted between the arm's real instructions: the arm returned with ebp one
higher, `leave` left esp a byte off, and the caller's next push landed on its
own std::string `this`.
"""

import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

from tools.recomp.disasm import Disassembler  # noqa: E402

BASE = 0x0030C87A
ARM = 0x0030C88C
IMAGE = bytes.fromhex(
    "8bff"                                   # 0x0030C87A mov edi, edi
    "8cc83000" "94c83000" "a0c83000" "b4c83000"  # 0x0030C87C table
    "8b4508"                                 # 0x0030C88C mov eax, [ebp+8]
    "5e" "5f" "c9" "c3")                     # pop esi; pop edi; leave; ret


def _decode():
    return Disassembler().disassemble_function(
        IMAGE, BASE, BASE + len(IMAGE), resync=[ARM])


def test_the_arm_is_decoded_on_its_own_boundaries():
    insns = _decode()
    at = {i.address: i for i in insns}
    arm = [a for a in sorted(at) if a >= ARM]
    assert arm == [ARM, ARM + 3, ARM + 4, ARM + 5, ARM + 6], \
        [(hex(a), at[a].mnemonic) for a in arm]
    assert [at[a].mnemonic for a in arm] == \
        ["mov", "pop", "pop", "leave", "ret"], [at[a].mnemonic for a in arm]
    print("ok  the_arm_is_decoded_on_its_own_boundaries")


def test_no_two_instructions_overlap_past_the_table():
    insns = [i for i in _decode() if i.address >= ARM]
    for a, b in zip(insns, insns[1:]):
        assert a.address + a.size <= b.address, (hex(a.address), hex(b.address))
    print("ok  no_two_instructions_overlap_past_the_table")


def test_the_premise_a_linear_decode_is_out_of_phase():
    # Without the resync point the sweep really does start an instruction
    # inside the arm's first one; otherwise the test above proves nothing.
    insns = Disassembler().disassemble_function(IMAGE, BASE, BASE + len(IMAGE))
    starts = {i.address for i in insns}
    assert ARM not in starts and (ARM + 1) in starts, sorted(map(hex, starts))
    print("ok  the_premise_a_linear_decode_is_out_of_phase")


if __name__ == "__main__":
    test_the_premise_a_linear_decode_is_out_of_phase()
    test_the_arm_is_decoded_on_its_own_boundaries()
    test_no_two_instructions_overlap_past_the_table()
    print("\nall passed")
