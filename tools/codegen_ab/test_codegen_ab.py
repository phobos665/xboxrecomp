"""Smoke test: the A/B runner builds, runs and agrees on a small corpus.

Needs a POSIX host, gcc or clang and binutils; skipped otherwise. The full
corpus (python3 -m tools.codegen_ab) is the real check and takes a minute.
"""

import os
import shutil
import tempfile
import unittest

from . import __main__ as ab


class SmokeTest(unittest.TestCase):
    def test_small_corpus_agrees(self):
        cc = shutil.which("gcc") or shutil.which("clang")
        if os.name != "posix" or not cc or not shutil.which("as") \
                or not shutil.which("ld"):
            self.skipTest("needs a POSIX host with a C compiler and binutils")
        tmp = tempfile.mkdtemp()
        try:
            rc = ab.main(["--cc", os.path.basename(cc), "--opts", "all",
                          "--states", "2", "--generated", "12",
                          "--sources", "bench,generated", "--workdir", tmp])
            self.assertEqual(rc, 0)
        finally:
            shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    unittest.main()
