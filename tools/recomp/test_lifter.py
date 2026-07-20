"""
Self-check for __SEH_prolog / __SEH_epilog detection.

Run: py -3 tools/recomp/test_lifter.py

Regression guard for the bug where the two addresses were class constants
holding Burnout 3's values. Every other title silently got no frame pointer
set up after a __SEH_prolog call: ebp kept whatever stale value it had and the
first ebp-relative local read through it.
"""

import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

from tools.recomp import config  # noqa: E402
from tools.recomp.lifter import detect_seh_helpers  # noqa: E402


# One section, VA 0x10000 -> file offset 0. Keeps the fixture arithmetic
# readable: address 0x10000 + n is byte n of the blob.
BASE = 0x00010000


def _install_layout():
    config._install(
        [config.Section(".text", BASE, 0x1000, 0x0000, 0x1000, True)],
        entry_point=BASE, kernel_thunk_addr=BASE, origin="lifter-test")


PROLOG = (b"\x64\xa1\x00\x00\x00\x00"      # mov eax, fs:[0]
          b"\x8d\x6c\x24\x10"              # lea ebp, [esp+0x10]
          b"\xc3")
EPILOG = (b"\x64\x89\x0d\x00\x00\x00\x00"  # mov fs:[0], ecx
          b"\xc9\x51\xc3")                 # leave; push ecx; ret
DECOY = b"\x55\x8b\xec\x83\xec\x40\xc3"    # ordinary push ebp; mov ebp,esp


def _blob(*chunks):
    """Lay chunks out back to back; return (bytes, [(addr, size), ...])."""
    data, placed, off = bytearray(), [], 0
    for c in chunks:
        placed.append((BASE + off, len(c)))
        data += c
        off += len(c)
    return bytes(data), placed


def _func_db(placed):
    return {addr: {"size": size} for addr, size in placed}


def test_finds_both_helpers():
    _install_layout()
    data, placed = _blob(DECOY, PROLOG, EPILOG)
    prolog, epilog = detect_seh_helpers(_func_db(placed), data)
    assert prolog == placed[1][0], hex(prolog or 0)
    assert epilog == placed[2][0], hex(epilog or 0)
    print("ok  finds_both_helpers")


def test_absent_helpers_are_none():
    _install_layout()
    data, placed = _blob(DECOY, DECOY)
    assert detect_seh_helpers(_func_db(placed), data) == (None, None)
    print("ok  absent_helpers_are_none")


def test_end_as_hex_string():
    # functions.json stores "end" as a hex string and omits "size"; the batch
    # translator later rewrites it to an int in place, so both must work.
    _install_layout()
    data, placed = _blob(PROLOG)
    addr, size = placed[0]
    for end in (f"0x{addr + size:08X}", addr + size):
        db = {addr: {"end": end}}
        assert detect_seh_helpers(db, data)[0] == addr, repr(end)
    print("ok  end_as_hex_string")


def test_oversized_match_is_rejected():
    # A big function that happens to touch fs:[0] is not the CRT helper.
    _install_layout()
    padding = b"\x90" * 200
    data, placed = _blob(PROLOG + padding)
    assert detect_seh_helpers(_func_db(placed), data)[0] is None
    print("ok  oversized_match_is_rejected")


def test_unmapped_or_missing_bytes():
    _install_layout()
    data, placed = _blob(PROLOG)
    # Address outside every section: va_to_file_offset returns None.
    assert detect_seh_helpers({0x7FFFFFFF: {"size": 11}}, data) == (None, None)
    # No XBE bytes at all (callers may construct a Lifter without them).
    assert detect_seh_helpers(_func_db(placed), None) == (None, None)
    print("ok  unmapped_or_missing_bytes")


if __name__ == "__main__":
    test_finds_both_helpers()
    test_absent_helpers_are_none()
    test_end_as_hex_string()
    test_oversized_match_is_rejected()
    test_unmapped_or_missing_bytes()
    print("\nall passed")
