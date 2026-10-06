"""
Self-check for 16-bit push and pop.

Run: py -3 tools/recomp/test_push_pop16.py

`pop bx` (66 5B) was lifted as `POP32(esp, bx);`, which names no C variable,
so the whole title failed to compile (WWE Raw 2, sub_00105B10 -- a data table
the disassembler decoded as code, but the lifter has to emit valid C for it
either way). An operand-size prefix makes push and pop move two bytes and
touch only the low half of the register.
"""

import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

from tools.recomp.lifter import Lifter  # noqa: E402


class _Op:
    def __init__(self, text, type="reg", reg=None, size=4, mem_size=None, imm=0,
                 base=None, index=None, scale=1, disp=0):
        self.text = text
        self.type = type
        self.reg = reg
        self.size = size
        self.mem_size = mem_size
        self.imm = imm
        self.base = base
        self.index = index
        self.scale = scale
        self.disp = disp
        self.segment = None


class _Insn:
    def __init__(self, mnemonic, operands, address=0x00100000, size=2):
        self.mnemonic = mnemonic
        self.operands = operands
        self.address = address
        self.size = size
        self.call_target = None


def _pop(op):
    return "\n".join(Lifter()._lift_pop(_Insn("pop", [op]), [op]))


def _push(op):
    return "\n".join(Lifter()._lift_push(_Insn("push", [op]), [op]))


def test_pop_r16_takes_two_bytes_into_the_low_half():
    out = _pop(_Op("bx", reg="bx", size=2))
    assert "POP32" not in out, out
    assert "MEM16(esp)" in out and "esp += 2" in out, out
    assert "SET_LO16(ebx, _tmp)" in out, out


def test_push_r16_moves_two_bytes():
    out = _push(_Op("bx", reg="bx", size=2))
    assert "PUSH32" not in out, out
    assert "esp -= 2" in out and "MEM16(esp) = _pv" in out, out


def test_pop_r32_is_unchanged():
    assert _pop(_Op("ebx", reg="ebx")) == "POP32(esp, ebx);"


def test_push_r32_is_unchanged():
    assert _push(_Op("ebx", reg="ebx")) == "PUSH32(esp, ebx);"


def test_segment_registers_keep_a_four_byte_slot():
    assert "POP32(esp, _tmp)" in _pop(_Op("fs", reg="fs", size=2))
    assert _push(_Op("fs", reg="fs", size=2)).startswith("PUSH32(")


if __name__ == "__main__":
    for name, fn in sorted(globals().items()):
        if name.startswith("test_") and callable(fn):
            fn()
            print("ok", name)
