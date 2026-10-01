"""--perf-opts leaf-cache: registers in locals for functions that call nothing.

The semantic check is tools.codegen_ab (the same functions with and without
the option, over the same machines). These pin which functions qualify, what
a cached function looks like, and that the register macros are back in force
for whatever follows it in the translation unit.
"""

import os
import shutil
import subprocess
import tempfile
import unittest

from . import leaf_cache
from .test_perf_opts import _Lift, RUNTIME


class LeafCacheShapeTest(_Lift):
    LEAF = "8B4424048B4C2408F7E1C20800"   # mov eax,[esp+4]; mov ecx,[esp+8]; mul ecx; ret 8

    def test_off_is_unchanged(self):
        self.assertNotIn("leaf-cache", self.lift(self.LEAF))

    def test_a_leaf_keeps_its_registers_in_locals(self):
        c = self.lift(self.LEAF, {"leaf-cache"})
        self.assertIn("uint32_t eax = g_eax, ecx = g_ecx, edx = g_edx, esp = g_esp;", c)
        self.assertIn("const ptrdiff_t _base = g_xbox_mem_offset;", c)
        self.assertIn("{ g_eax = eax; g_ecx = ecx; g_edx = edx; g_esp = esp; return; }", c)
        # Only the registers it names.
        self.assertNotIn("g_ebx", c)
        # The aliases are restored after the function.
        tail = c[c.rindex("\n}"):]
        for r in ("eax", "ecx", "edx", "esp"):
            self.assertIn(f"#define {r} g_{r}", tail)
        self.assertIn("#define XBOX_PTR(addr) ((uintptr_t)(uint32_t)(addr) + "
                      "g_xbox_mem_offset)", tail)

    def test_a_function_that_calls_is_left_alone(self):
        # call +0; ret
        code = "E800000000C3"
        self.assertEqual(self.lift(code, {"leaf-cache"}), self.lift(code))

    def test_why_not(self):
        self.assertIsNone(leaf_cache.why_not(
            "void f(void)\n{\n    eax = MEM32(esp + 4); esp += 4; return;\n}\n"))
        self.assertEqual(leaf_cache.why_not(
            "void f(void)\n{\n    g_eax = 1;\n}\n"), "names a register global directly")
        self.assertEqual(leaf_cache.why_not(
            "void f(void)\n{\n    RECOMP_ICALL(eax);\n}\n"), "calls RECOMP_ICALL")
        self.assertEqual(leaf_cache.why_not(
            "void f(void)\n{\n    sub_00012345();\n}\n"), "calls sub_00012345")
        self.assertEqual(leaf_cache.why_not(
            "void f(void)\n{\n    recomp_int3_reached(0x10u);\n}\n"),
            "calls recomp_int3_reached")
        # A name only in a comment does not count; a cast is not a call.
        self.assertIsNone(leaf_cache.why_not(
            "void f(void)\n{\n    eax = (uint32_t)(ecx); /* sub_00012345() */\n}\n"))

    def test_cached_and_uncached_functions_compile_side_by_side(self):
        cc = shutil.which("gcc") or shutil.which("clang")
        if not cc:
            self.skipTest("no C compiler")
        cached = self.lift(self.LEAF, {"leaf-cache"})
        self.assertIn("leaf-cache", cached)
        src = ("#define RECOMP_GENERATED_CODE\n#include \"recomp_types.h\"\n"
               + cached.replace("sub_00011000", "cached_fn")
               # After the cached function `eax` must be the global again.
               + "uint32_t *after(void) { return &eax; }\n"
               "uint32_t *global(void) { return &g_eax; }\n")
        tmp = tempfile.mkdtemp()
        try:
            path = os.path.join(tmp, "t.c")
            with open(path, "w") as f:
                f.write(src)
            r = subprocess.run([cc, "-O2", "-Wall", "-Werror", "-Wno-unused-label",
                                "-Wno-unused-function", "-Wno-unused-variable",
                                "-Wno-unused-but-set-variable", "-c", "-I", RUNTIME,
                                path, "-o", os.path.join(tmp, "t.o")],
                               capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stderr[-3000:])
        finally:
            shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    unittest.main()
