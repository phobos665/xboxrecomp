"""recomp_types_simd.h: which generated files skip it, and that skipping is safe.

recomp_types.h includes recomp_types_simd.h unless RECOMP_NO_SIMD is defined,
and the lifter defines it in a file that names nothing the SIMD header
defines (runtime_headers.py). These pin the three things that make that
safe: the name list really is the header's, the scan finds a use and does
not invent one, and a file that wrongly opts out fails to compile rather
than building something different.
"""

import os
import re
import shutil
import subprocess
import tempfile
import unittest

from tools.recomp import runtime_headers as rh
from tools.recomp.hle import render_thunks

CORE = os.path.join(rh.RUNTIME_DIR, "recomp_types.h")
SIMD = os.path.join(rh.RUNTIME_DIR, rh.SIMD_HEADER)


def _cc():
    return shutil.which("clang") or shutil.which("gcc") or shutil.which("cc")


class SimdNamesTest(unittest.TestCase):
    def test_names_cover_the_model(self):
        names = rh.simd_names()
        for want in ("XMM_ADD", "XMM_MEM", "XMM_LOAD_LOW", "XMM_CMP_PRED",
                     "MMX_PADDB", "MMX_PSRAW", "MMX_CVT_F2I", "XMM_FROM_PI",
                     "recomp_cvtss2si", "recomp_cvttsd2si", "recomp_sat_u8",
                     "RecompXmm", "RecompMmx", "g_xmm0", "g_mm7", "xmm7", "mm0"):
            self.assertIn(want, names)

    def test_no_words_that_are_not_definitions(self):
        names = rh.simd_names()
        for word in ("name", "NAME", "expr", "defined", "if", "for", "switch",
                     "RECOMP_TYPES_SIMD_H", "MEM32", "XBOX_PTR"):
            self.assertNotIn(word, names)

    def test_core_header_defines_none_of_them(self):
        """A helper defined in both would be a redefinition for every chunk
        that includes both, and invisible to the scan for the ones that skip."""
        core = open(CORE, encoding="utf-8").read()
        defined = set(re.findall(r"^\s*#\s*define\s+(\w+)", core, re.M))
        defined.update(re.findall(r"\b(\w+)\s*\([^;{}()]*\)\s*\{", core))
        self.assertEqual(sorted(defined & rh.simd_names()), [])

    def test_core_includes_it_unless_opted_out(self):
        core = open(CORE, encoding="utf-8").read()
        self.assertRegex(core, r"#ifndef RECOMP_NO_SIMD\s*\n#include \"recomp_types_simd.h\"")


class UsesSimdTest(unittest.TestCase):
    def test_finds_uses(self):
        for text in ("xmm0 = XMM_ADD(xmm0, xmm1);",
                     "g_mm3 = MMX_PXOR(g_mm3, g_mm3);",
                     "eax = (uint32_t)recomp_cvttss2si(MEMF(ecx));",
                     "    mm2 = MMX_MEM(esp + 8);\n"):
            self.assertTrue(rh.uses_simd(text), text)

    def test_ignores_lookalikes(self):
        for text in ("eax = MEM32(esp + 4); /* commit the command */",
                     "sub_XMM_ADD2(); mm8 = 0; my_mm0 = 1;",
                     "ecx = SX16(LO16(eax)); RECOMP_ICALL(eax);",
                     ""):
            self.assertFalse(rh.uses_simd(text), text)

    def test_opt_out_line(self):
        self.assertEqual(rh.simd_opt_out("eax = 0;"), [rh.NO_SIMD_DEFINE])
        self.assertEqual(rh.simd_opt_out("xmm0 = XMM_ZERO();"), [])

    def test_hle_thunks_opt_out(self):
        text = render_thunks({0x00011000: ("D3DDevice_Swap", 4, None)})
        lines = text.split("\n")
        at = lines.index('#include "recomp_types.h"')
        self.assertEqual(lines[at - 1], rh.NO_SIMD_DEFINE)


class RefreshTest(unittest.TestCase):
    def test_copies_each_header_once(self):
        with tempfile.TemporaryDirectory() as d:
            sink = open(os.devnull, "w")
            self.assertEqual(rh.refresh_runtime_headers(d, out=sink),
                             list(rh.RUNTIME_HEADERS))
            self.assertEqual(rh.refresh_runtime_headers(d, out=sink), [])
            with open(os.path.join(d, rh.SIMD_HEADER), "a") as fh:
                fh.write("/* local edit */\n")
            self.assertEqual(rh.refresh_runtime_headers(d, out=sink),
                             [rh.SIMD_HEADER])
            sink.close()


@unittest.skipUnless(_cc(), "no C compiler")
class CompileTest(unittest.TestCase):
    def _compile(self, body, opt_out):
        with tempfile.TemporaryDirectory() as d:
            src = os.path.join(d, "chunk.c")
            with open(src, "w") as fh:
                fh.write("#define RECOMP_GENERATED_CODE\n")
                if opt_out:
                    fh.write(rh.NO_SIMD_DEFINE + "\n")
                fh.write('#include "recomp_types.h"\n#include <math.h>\n')
                fh.write("void f(void) {\n" + body + "\n}\n")
            return subprocess.run(
                [_cc(), "-std=gnu11", "-Werror=implicit-function-declaration",
                 "-I", rh.RUNTIME_DIR, "-c", src, "-o", os.path.join(d, "c.o")],
                capture_output=True, text=True)

    def test_opted_out_chunk_builds(self):
        r = self._compile("PUSH32(esp, ebx); eax = ROL32(eax, 3); "
                          "RECOMP_ICALL(eax); POP32(esp, ebx);", True)
        self.assertEqual(r.returncode, 0, r.stderr)

    def test_full_chunk_builds(self):
        r = self._compile("xmm0 = XMM_ADD(xmm0, XMM_MEM(eax)); "
                          "mm0 = MMX_PADDB(mm0, MMX_MEM(ecx)); "
                          "eax = (uint32_t)recomp_cvtss2si(MEMF(edx));", False)
        self.assertEqual(r.returncode, 0, r.stderr)

    def test_wrong_opt_out_fails_loudly(self):
        r = self._compile("xmm0 = XMM_ADD(xmm0, xmm1);", True)
        self.assertNotEqual(r.returncode, 0)


if __name__ == "__main__":
    unittest.main()
