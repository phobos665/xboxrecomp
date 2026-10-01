"""
Self-check for function-end detection around out-of-line tails.

Run: py -3 tools/disasm/test_function_end.py

Regression guard for the bug where a forward branch target was stored in
max_addr and then used as an *exclusive* end. The coverage test
`addr + size >= max_addr` reported "covered" while sitting exactly on the
target, so the function ended at the first instruction it was required to
contain.

MSVC emits this shape constantly: a conditional branch forward, a body, an
unconditional `jmp` backwards, then the branch target parked out of line
after it. Halo's get_edge_vertex (0x00107EC0) ended at 0x00107FB8 -- its own
tail -- so the tail was lifted as a separate function and the `jne` to it
became a tail call that returned without running the epilogue, leaking the
whole 28-byte frame on every call. Three calls in and the caller's saved
esi/edi/ebx came back as garbage.
"""

import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

from tools.disasm.functions import FunctionDetector  # noqa: E402


class _Insn:
    def __init__(self, addr, size, mnemonic="mov", target=None,
                 is_ret=False, is_jump=False, is_cond_jump=False,
                 jump_table=None):
        self.address = addr
        self.size = size
        self.end_address = addr + size
        self.mnemonic = mnemonic
        self.jump_target = target
        self.jump_table = jump_table
        self.is_ret = is_ret
        self.is_jump = is_jump
        self.is_cond_jump = is_cond_jump
        self.is_branch = is_jump or is_cond_jump


class _Engine:
    def __init__(self, insns, jump_tables=None, entries=None):
        self.by_addr = {i.address: i for i in insns}
        self.jump_tables = jump_tables or {}
        self.entries = entries or {}

    def get_instruction(self, addr):
        return self.by_addr.get(addr)

    def jump_table_entries(self, tbl):
        return self.entries.get(tbl, [])


def _detector(insns, jump_tables=None, entries=None):
    det = FunctionDetector.__new__(FunctionDetector)
    det.engine = _Engine(insns, jump_tables, entries)
    return det


# The real shape of Halo get_edge_vertex's tail, addresses preserved.
#   0x107F9F  test dx, dx
#   0x107FA2  jne  0x107FB8      <- forward branch over the epilogue
#   0x107FA4  ...epilogue...
#   0x107FAD  ret
#   0x107FAE  mov ebx, esi
#   0x107FB3  jmp  0x107EE5      <- unconditional, backwards
#   0x107FB8  cmp dx, [edi]      <- the branch target, out of line
#   0x107FC1  ret
TAIL = [
    _Insn(0x107F9F, 3),
    _Insn(0x107FA2, 2, "jne", target=0x107FB8, is_cond_jump=True),
    _Insn(0x107FA4, 9),
    _Insn(0x107FAD, 1, "ret", is_ret=True),
    _Insn(0x107FAE, 5),
    _Insn(0x107FB3, 5, "jmp", target=0x107EE5, is_jump=True),
    _Insn(0x107FB8, 9),
    _Insn(0x107FC1, 1, "ret", is_ret=True),
]


def test_out_of_line_tail_is_included():
    det = _detector(TAIL)
    end = det._find_function_end(0x107F9F, next_func=None, sec_end=0x108100)
    assert end > 0x107FB8, f"function cut off at its own branch target: {end:#x}"
    assert end == 0x107FC2, f"expected end past the tail's ret, got {end:#x}"


def test_plain_function_still_ends_at_ret():
    # No forward branches: the first ret ends it. Guards against the fix
    # running functions together.
    insns = [_Insn(0x1000, 4), _Insn(0x1004, 1, "ret", is_ret=True),
             _Insn(0x1005, 4)]
    det = _detector(insns)
    end = det._find_function_end(0x1000, next_func=None, sec_end=0x2000)
    assert end == 0x1005, f"expected {0x1005:#x}, got {end:#x}"


def test_next_function_still_bounds_the_walk():
    det = _detector(TAIL)
    end = det._find_function_end(0x107F9F, next_func=0x107FB0, sec_end=0x108100)
    assert end <= 0x107FB0, f"walked past the next function: {end:#x}"


def test_embedded_jump_table_is_stepped_over():
    # MSVC's memcpy shape: a switch dispatch, the table inline right after it,
    # then the tail-copy cases and the epilogue. resync_jump_tables() has
    # already removed the instructions the sweep hallucinated over the table,
    # so there is a hole at 0x2010 and the walk must jump it.
    insns = [
        _Insn(0x2000, 3),
        _Insn(0x2003, 7, "jmp", is_jump=True, jump_table=0x2010),
        # 0x2010..0x201F: four table entries, no instructions
        _Insn(0x2020, 4),   # a case body, reached only through the table
        _Insn(0x2024, 1, "pop"),
        _Insn(0x2025, 1, "ret", is_ret=True),
    ]
    det = _detector(insns, jump_tables={0x2010: 0x2020},
                    entries={0x2010: [0x2020, 0x2020, 0x2024, 0x2024]})
    end = det._find_function_end(0x2000, next_func=None, sec_end=0x3000)
    assert end == 0x2026, (
        f"function ended at its own switch table instead of its epilogue: "
        f"{end:#x}")


def test_jump_table_past_next_function_is_ignored():
    # A table recorded beyond the bounds must not drag the function over its
    # neighbour.
    insns = [_Insn(0x2000, 3), _Insn(0x2003, 1, "ret", is_ret=True)]
    det = _detector(insns, jump_tables={0x2100: 0x2200})
    end = det._find_function_end(0x2000, next_func=0x2050, sec_end=0x3000)
    assert end == 0x2004, f"expected {0x2004:#x}, got {end:#x}"


# A gap_prologue start inside a function found later is absorbed.
#
# Jet Set Radio Future's sub_00025040: eleven conditional jumps from the body
# land on a shared epilogue parked after an early ret, and the byte after that
# ret began with a prologue shape. _pass_gap_prologues saw a gap there (the
# function itself was only found by the later tail-jump alias pass) and made
# it a start; clamped to it, the body's own jumps past it were lifted as tail
# calls to stubs, and the title skipped its epilogue.
def _gap_detector():
    insns = [
        _Insn(0x1000, 2),                                   # push esi...
        _Insn(0x1002, 6, "jb", target=0x1030, is_cond_jump=True),
        _Insn(0x1008, 2),
        _Insn(0x100A, 1, "ret", is_ret=True),
        _Insn(0x100B, 2),                                   # the "prologue"
        _Insn(0x100D, 5),
        _Insn(0x1012, 1, "ret", is_ret=True),
        _Insn(0x1013, 0x1D),                                # padding, decoded
        _Insn(0x1030, 2),                                   # shared epilogue
        _Insn(0x1032, 1, "ret", is_ret=True),
        _Insn(0x1040, 1, "ret", is_ret=True),               # the real next function
    ]
    det = _detector(insns)
    det._candidates = {0x1000: (0.9, "tail_jump_alias"),
                       0x100B: (0.85, "gap_prologue"),
                       0x1040: (0.9, "call_target")}
    det.functions = {}
    det._absorbed = set()
    return det


def test_body_jumping_past_the_gap_start_absorbs_it():
    det = _gap_detector()
    end = det._find_function_end(0x1000, next_func=0x100B, sec_end=0x2000)
    assert end == 0x1033, f"expected {0x1033:#x}, got {end:#x}"
    assert 0x100B not in det._candidates, "the gap start is dropped"
    assert 0x100B in det._absorbed


def test_a_gap_start_nothing_jumps_past_is_kept():
    det = _gap_detector()
    det.engine.by_addr[0x1002] = _Insn(0x1002, 6, "jb", target=0x1008,
                                       is_cond_jump=True)
    end = det._find_function_end(0x1000, next_func=0x100B, sec_end=0x2000)
    assert end == 0x100B, f"expected {0x100B:#x}, got {end:#x}"
    assert 0x100B in det._candidates


def test_a_tail_call_to_the_gap_start_keeps_it():
    # The body jumps *to* the start, not past it: a tail call to a real
    # function, which must stay a function. (Absorbing on "the walk ran
    # past it" alone cost Jet Set Radio Future 252 functions.)
    det = _gap_detector()
    det.engine.by_addr[0x1002] = _Insn(0x1002, 6, "jmp", target=0x100B,
                                       is_jump=True)
    end = det._find_function_end(0x1000, next_func=0x100B, sec_end=0x2000)
    # `push; jmp foo` ends at the jmp, and the clamp stands.
    assert end == 0x1008, f"expected {0x1008:#x}, got {end:#x}"
    assert 0x100B in det._candidates


def test_a_conditional_jump_to_the_gap_start_absorbs_it():
    # Compiled code never tail-calls through a jcc, so a `je` *to* the start
    # is an internal branch: JSRF's sub_00025040 reaches 0x25233 with one.
    det = _gap_detector()
    det.engine.by_addr[0x1002] = _Insn(0x1002, 6, "je", target=0x100B,
                                       is_cond_jump=True)
    end = det._find_function_end(0x1000, next_func=0x100B, sec_end=0x2000)
    # Nothing reaches 0x1030 here, so the body ends at the absorbed code's ret.
    assert end == 0x1013, f"expected {0x1013:#x}, got {end:#x}"
    assert 0x100B in det._absorbed


def test_a_switch_case_past_the_gap_start_absorbs_it():
    det = _gap_detector()
    det.engine.by_addr[0x1002] = _Insn(0x1002, 6, "jmp", is_jump=True,
                                       jump_table=0x1800)
    det.engine.entries[0x1800] = [0x1008, 0x1030]
    end = det._find_function_end(0x1000, next_func=0x100B, sec_end=0x2000)
    assert end == 0x1033, f"expected {0x1033:#x}, got {end:#x}"
    assert 0x100B in det._absorbed


def test_an_absorbed_start_bounds_nothing():
    det = _gap_detector()
    det._absorbed.add(0x100B)
    det._candidates.pop(0x100B)
    end = det._find_function_end(0x1000, next_func=0x100B, sec_end=0x2000)
    assert end == 0x1033, f"expected {0x1033:#x}, got {end:#x}"


def test_an_alias_body_keeps_the_measurement_past_an_absorbed_clamp():
    # The alias passes clamp to the next known start and only ever accepted
    # a *shorter* measurement, so an absorbed clamp was dropped from the
    # function list while the alias still ended on it.
    det = _gap_detector()
    assert not det._measured_extends(0x1000, 0x100B, 0x1033)
    assert det._measured_extends(0x1000, 0x100B, 0x1008)
    det._absorbed.add(0x100B)
    assert det._measured_extends(0x1000, 0x100B, 0x1033)
    assert not det._measured_extends(0x1000, 0x100B, None)


def test_a_start_found_by_another_pass_still_clamps():
    det = _gap_detector()
    det._candidates[0x100B] = (0.9, "call_target")
    end = det._find_function_end(0x1000, next_func=0x100B, sec_end=0x2000)
    assert end == 0x100B, f"expected {0x100B:#x}, got {end:#x}"


if __name__ == "__main__":
    failures = 0
    for name, fn in sorted(globals().items()):
        if not name.startswith("test_"):
            continue
        try:
            fn()
            print(f"  ok   {name}")
        except AssertionError as exc:
            failures += 1
            print(f"  FAIL {name}: {exc}")
    print("function end: " + ("OK" if not failures else f"{failures} FAILED"))
    sys.exit(1 if failures else 0)

