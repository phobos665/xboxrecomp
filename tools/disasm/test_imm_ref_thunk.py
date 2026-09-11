"""
Self-check for adjustor thunks in FunctionDetector._pass_imm_ref_targets.

Run: py -3 tools/disasm/test_imm_ref_thunk.py

A function pointer written into an object field as an immediate is found by
the immediate pass only if its body reaches a ret. A thunk that adjusts an
argument and tail-jumps into a real function never does, so it was dropped --
Burnout 2's stream callback at 0x00110440 was, while its two neighbours
installed by the same constructor were found.
"""

import os
import sys
from types import SimpleNamespace

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

from tools.disasm.engine import DisasmEngine  # noqa: E402
from tools.disasm.functions import FunctionDetector  # noqa: E402
from tools.disasm.test_alias_tail_jumps import _Img, _rel32  # noqa: E402
from tools.disasm.test_decode_at import _Section  # noqa: E402

BASE = 0x00010000


def _detector(jump_to):
    """
    BASE+0x00  a real function:  ret
    BASE+0x10  the installer:    mov dword [esi+0x10], BASE+0x30; ret
    BASE+0x30  the thunk:        mov eax,[esp+4]; mov ecx,[eax+0x4c];
                                 mov [esp+4],ecx; jmp <jump_to>
    """
    code = bytearray(b"\xcc" * 0x60)
    code[0x00] = 0xc3
    code[0x10:0x17] = b"\xc7\x46\x10" + (BASE + 0x30).to_bytes(4, "little")
    code[0x17] = 0xc3
    body = b"\x8b\x44\x24\x04\x8b\x48\x4c\x89\x4c\x24\x04"
    code[0x30:0x30 + len(body)] = body
    end = BASE + 0x30 + len(body) + 5
    code[0x30 + len(body):0x30 + len(body) + 5] = b"\xe9" + _rel32(end, jump_to)
    code = bytes(code)

    sec = _Section(BASE, code)
    sec.data = code
    engine = DisasmEngine(_Img(sec))
    engine.linear_sweep(sec)
    det = FunctionDetector.__new__(FunctionDetector)
    det.engine = engine
    det.image = engine.image
    det._candidates = {}
    det._alias_entries = {}
    det.functions = {
        BASE: SimpleNamespace(start=BASE, end=BASE + 1),
        BASE + 0x10: SimpleNamespace(start=BASE + 0x10, end=BASE + 0x18),
    }
    return det, sec


def test_a_thunk_into_a_known_function_is_found():
    det, sec = _detector(BASE)
    assert det._pass_imm_ref_targets([sec])
    assert BASE + 0x30 in det._candidates, det._candidates


def test_a_jump_to_nowhere_known_is_not_enough():
    det, sec = _detector(BASE + 0x58)       # int3 padding, not a start
    det._pass_imm_ref_targets([sec])
    assert BASE + 0x30 not in det._candidates, det._candidates


if __name__ == "__main__":
    for name, fn in list(globals().items()):
        if name.startswith("test_"):
            fn()
            print("ok", name)
