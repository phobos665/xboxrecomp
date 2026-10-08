"""A lift spread over worker processes writes the same files as a serial one.

BatchTranslator.translate_batch_split(jobs=N) lifts in a spawn-started process
pool and puts the results back in function order. That is only safe while
lifting one function reads nothing another one wrote; the Lifter's running
tallies (call targets named, instructions left unimplemented) are carried
back from the workers and replayed. This lifts a small synthetic XBE both
ways and compares every generated file byte for byte, so a change that makes
one function's output depend on an earlier one's -- or a tally the replay
misses -- shows up here rather than as a title that builds differently on a
machine with a different core count.
"""

import json
import os
import struct
import sys
import tempfile
import unittest
from unittest import mock

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
sys.path.insert(0, os.path.join(REPO, "scripts"))

from tools.recomp import config  # noqa: E402
from tools.recomp.translator import (BatchTranslator,  # noqa: E402
                                     FunctionTranslator, _CONFIG_STATE)

import make_test_xbe  # noqa: E402

TEXT_VA = make_test_xbe.BASE + make_test_xbe.HEADER_SIZE      # 0x00011000
TEXT_RAW = make_test_xbe.HEADER_SIZE
UNRESOLVED = TEXT_VA + 0x7F00     # zeros: called, never a function


def _rel32(at, size, target):
    return struct.pack("<i", target - (at + size))


def _code():
    """{start: bytes} for a handful of functions that exercise the tallies."""
    funcs = {}
    leaf = TEXT_VA + 0x20
    # A loop with a carry reader, and a call to an address that is not a
    # function (an unresolved stub).
    body = bytes.fromhex("33C0" "B90A000000" "03C1" "49" "75FB" "13C2")
    at = leaf + len(body)
    body += b"\xE8" + _rel32(at, 5, UNRESOLVED) + b"\xC3"
    funcs[leaf] = body
    # A frame, a branch and a call to the leaf.
    entry = TEXT_VA
    body = bytes.fromhex("558BEC" "8B4508" "85C0" "7405")
    body += b"\xE8" + _rel32(entry + len(body), 5, leaf) + bytes.fromhex("5DC3")
    funcs[entry] = body
    # x87, an unimplemented instruction (insb) and an indirect call.
    funcs[TEXT_VA + 0x40] = bytes.fromhex("D9442404" "D8C0" "D95C2404"
                                          "6C" "FFD0" "C3")
    # Forty more that call each other in a chain, some with an
    # unimplemented instruction, so the work spreads over several units.
    prev = leaf
    for k in range(40):
        start = TEXT_VA + 0x100 + k * 0x20
        body = b"\xB8" + struct.pack("<I", k)
        body += b"\xE8" + _rel32(start + len(body), 5, prev)
        if k % 3 == 0:
            body += b"\x6C"                      # insb
        if k % 4 == 1:
            body += bytes.fromhex("85C07402" "33C0")   # test; je; xor
        body += b"\xC3"
        funcs[start] = body
        prev = start
    return funcs


class ParallelLiftIsByteIdentical(unittest.TestCase):

    def setUp(self):
        self._saved = {k: getattr(config, k) for k in _CONFIG_STATE}
        self.tmp = tempfile.TemporaryDirectory()
        d = self.tmp.name
        image = bytearray(make_test_xbe.build(title="Parallel Test"))
        db = []
        for start, body in sorted(_code().items()):
            off = TEXT_RAW + (start - TEXT_VA)
            image[off:off + len(body)] = body
            db.append({"start": f"0x{start:08X}",
                       "end": f"0x{start + len(body):08X}",
                       "size": len(body), "section": ".text",
                       "name": f"sub_{start:08X}", "has_prologue": False,
                       "calls_to": [], "called_by": []})
        self.xbe = os.path.join(d, "default.xbe")
        with open(self.xbe, "wb") as fh:
            fh.write(image)
        self.functions = os.path.join(d, "functions.json")
        with open(self.functions, "w") as fh:
            json.dump(db, fh)
        config.configure_from_xbe(self.xbe)

    def tearDown(self):
        for k, v in self._saved.items():
            setattr(config, k, v)
        self.tmp.cleanup()

    def _lift(self, out, jobs):
        bt = BatchTranslator(self.xbe, self.functions,
                             output_dir=os.path.join(self.tmp.name, "o"))
        funcs = bt.get_functions_by_category()
        # One function hand-written, one replaced but its body kept, as
        # __main__ does for recomp_manual.c and HLE_ORIGINAL.
        manual = {funcs[3][0], funcs[5][0]}
        keep = {funcs[5][0]: f"sub_{funcs[5][0]:08X}_hle_original"}
        stats = bt.translate_batch_split(funcs, out, chunk_size=7,
                                         manual=manual, keep_bodies=keep,
                                         jobs=jobs)
        files = {}
        for name in sorted(os.listdir(out)):
            with open(os.path.join(out, name), "rb") as fh:
                files[name] = fh.read()
        return stats, files

    def test_parallel_matches_serial(self):
        serial_stats, serial = self._lift(
            os.path.join(self.tmp.name, "serial"), jobs=1)
        # Nothing may be lifted in this process when jobs > 1: a quiet
        # fallback to the serial loop would pass the comparison below and
        # test nothing. Spawned workers import their own, unpatched copy.
        with mock.patch.object(FunctionTranslator, "translate_function",
                               side_effect=AssertionError("lifted in parent")):
            par_stats, par = self._lift(
                os.path.join(self.tmp.name, "parallel"), jobs=3)

        # The fixture has to exercise what the workers send back.
        self.assertTrue(serial_stats["unimplemented"])
        self.assertTrue(serial_stats["unresolved_stubs"])
        self.assertGreater(serial_stats["translated"], 40)
        self.assertIn("recomp_stubs_unresolved.c", serial)

        self.assertEqual(sorted(serial), sorted(par))
        for name in serial:
            self.assertEqual(serial[name], par[name], name)
        for key in ("translated", "failed", "total_lines",
                    "unresolved_stubs", "unimplemented", "num_chunks"):
            self.assertEqual(serial_stats[key], par_stats[key], key)


if __name__ == "__main__":
    unittest.main()
