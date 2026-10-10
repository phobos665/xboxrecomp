"""Self-check for incremental-link thunk tables.

Run: py -3 -m pytest tools/disasm/test_ilt_thunks.py

A build linked with /INCREMENTAL sends every function through a 5-byte
`jmp rel32` thunk, packed back to back at the start of .text, and every address
the program takes is the thunk's. MK Shaolin Monks has 4,753 of them at
0x00011005; discovery found the 2,689 that something calls directly, and its
title screen then made a virtual call through 0x00012EEB, one of the others,
which the runtime skipped.
"""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

from tools.disasm import config  # noqa: E402
from tools.disasm.functions import FunctionDetector  # noqa: E402

BASE = 0x00011000


class _Section:
    name = ".text"
    virtual_addr = BASE
    executable = True

    def __init__(self, size):
        self.virtual_size = size


class _Image:
    def __init__(self, data):
        self.data = data
        self.section = _Section(len(data))

    def get_section_data(self, section):
        return self.data

    def get_section_at_va(self, addr):
        if BASE <= addr < BASE + len(self.data):
            return self.section
        return None


class _Engine:
    def __init__(self, decoded):
        self.instructions = dict.fromkeys(decoded)

    def decode_at(self, addr):
        self.instructions[addr] = None
        return 1

    def probes_as_function_body(self, addr):
        return True


def _jmp(at, to):
    return b"\xE9" + (to - (at + 5)).to_bytes(4, "little", signed=True)


def _layout(n_thunks, start=0x5, body_at=0x1000):
    """<int3 x start><n thunks, thunk k -> body_at + 0x10*k><int3...>"""
    data = bytearray(b"\xCC" * (body_at + 0x10 * max(n_thunks, 1) + 0x10))
    for k in range(n_thunks):
        off = start + 5 * k
        data[off:off + 5] = _jmp(BASE + off, BASE + body_at + 0x10 * k)
    return bytes(data)


def _detect(data, decoded=()):
    det = FunctionDetector.__new__(FunctionDetector)
    det.engine = _Engine(decoded)
    det.image = _Image(data)
    found = {}
    det._add_candidate = lambda addr, conf, why: found.setdefault(addr, why)
    det._pass_jump_thunk_tables(det.image.section)
    return found


def test_every_entry_and_every_target_of_a_table_is_a_function():
    n = config.MIN_ILT_RUN + 4
    found = _detect(_layout(n))
    entries = [BASE + 5 + 5 * k for k in range(n)]
    targets = [BASE + 0x1000 + 0x10 * k for k in range(n)]
    assert all(found.get(e) == "ilt_thunk" for e in entries)
    assert all(found.get(t) == "ilt_target" for t in targets)


def test_a_short_run_is_not_a_table():
    found = _detect(_layout(config.MIN_ILT_RUN - 1))
    assert not found


def test_a_target_outside_code_ends_the_run():
    n = config.MIN_ILT_RUN * 2
    data = bytearray(_layout(n))
    # Entry 3 jumps out of every section: only the run after it is long enough.
    off = 5 + 5 * 3
    data[off:off + 5] = _jmp(BASE + off, 0x7FFF0000)
    found = _detect(bytes(data))
    assert BASE + off not in found
    assert BASE + 5 not in found            # the 3 before it are too few
    assert found.get(BASE + off + 5) == "ilt_thunk"


def test_an_entry_the_sweep_stepped_over_is_decoded():
    n = config.MIN_ILT_RUN
    det_found = _detect(_layout(n), decoded=())
    assert det_found.get(BASE + 5) == "ilt_thunk"
