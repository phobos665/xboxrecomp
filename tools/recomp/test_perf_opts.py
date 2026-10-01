"""--perf-opts: each option changes the spelling of an instruction, never what
it does, and none of them changes anything while it is off.

The semantic check for all of them together is tools.codegen_ab, which runs
the lifter's output with and without the options over the same guest machine.
These pin the shapes: what each option emits, and that a lift without it is
the long-standing output. The header test compiles both versions of the
packed-SSE helpers and compares them lane for lane, where a compiler is found.
"""

import os
import shutil
import struct
import subprocess
import tempfile
import unittest

from . import config, perf_opts
from .translator import FunctionTranslator, perf_defines

BASE = 0x00011000
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
RUNTIME = os.path.join(ROOT, "templates", "runtime")

_CONFIG_GLOBALS = (
    "_SECTIONS", "SECTIONS", "_configured_from",
    "TEXT_VA_START", "TEXT_VA_END", "RDATA_VA_START", "RDATA_VA_END",
    "DATA_VA_START", "DATA_VA_END", "KERNEL_THUNK_ADDR", "ENTRY_POINT",
)


class _Lift(unittest.TestCase):
    def setUp(self):
        self._saved = {k: getattr(config, k) for k in _CONFIG_GLOBALS}

    def tearDown(self):
        for k, v in self._saved.items():
            setattr(config, k, v)

    def lift(self, hexcode, opts=()):
        image = bytes.fromhex(hexcode)
        config._install(
            [config.Section(".text", BASE, len(image), 0, len(image), True)],
            entry_point=BASE, kernel_thunk_addr=BASE, origin="perf-opts-test")
        db = {BASE: {"start": f"0x{BASE:08X}", "end": BASE + len(image),
                     "_addr": BASE, "size": len(image)}}
        return FunctionTranslator(image, db, perf_opts=frozenset(opts)) \
            .translate_function(BASE, db[BASE])


class ParseTest(unittest.TestCase):
    def test_names(self):
        self.assertEqual(perf_opts.parse("all"), frozenset(perf_opts.OPTS))
        self.assertEqual(perf_opts.parse(""), frozenset())
        self.assertEqual(perf_opts.parse("none"), frozenset())
        self.assertEqual(perf_opts.parse("stosd, leaf-cache"),
                         frozenset({"stosd", "leaf-cache"}))
        with self.assertRaises(ValueError):
            perf_opts.parse("stosd,warp-speed")

    def test_cli_wins_over_environment(self):
        old = os.environ.get(perf_opts.ENV_VAR)
        try:
            os.environ[perf_opts.ENV_VAR] = "all"
            self.assertEqual(perf_opts.from_args(None), frozenset(perf_opts.OPTS))
            self.assertEqual(perf_opts.from_args("none"), frozenset())
            del os.environ[perf_opts.ENV_VAR]
            self.assertEqual(perf_opts.from_args(None), frozenset())
        finally:
            if old is not None:
                os.environ[perf_opts.ENV_VAR] = old


class StosTest(_Lift):
    def test_off_keeps_the_element_loop(self):
        c = self.lift("F3ABC3")                        # rep stosd; ret
        self.assertIn("for (_i = 0; _i < ecx; _i++) MEM32(edi + _i*_st) = eax;", c)
        self.assertNotIn("memset", c)

    def test_stosd_reads_registers_once_and_clears_with_memset(self):
        c = self.lift("F3ABC3", {"stosd"})
        self.assertIn("uint32_t _n = ecx, _v = eax, _d = edi;", c)
        self.assertIn("memset((void*)XBOX_PTR(_d), (int)(_v & 0xFFu), (size_t)_n * 4u)", c)
        # Only forwards, only a repeated byte, never into the apertures.
        self.assertIn("_st > 0 && (_v & 0xFFu) * 0x01010101u == _v", c)
        self.assertIn("<= 0xF0000000ull", c)
        self.assertIn("ecx = 0; /* rep stosd */", c)
        self.assertNotIn("_i < ecx", c)

    def test_stosw_stores_the_low_word(self):
        c = self.lift("66F3ABC3", {"stosd"})           # rep stosw; ret
        self.assertIn("_v = LO16(eax)", c)
        self.assertIn("MEM16(_d + _i*(uint32_t)_st) = _v;", c)
        self.assertIn("(_v & 0xFFu) * 0x0101u == _v", c)


class RmwSnapshotTest(_Lift):
    def test_off_reads_the_destination_back(self):
        c = self.lift("0106C3")                        # add [esi], eax; ret
        self.assertIn("MEM32(esi) = MEM32(esi) + eax;", c)
        self.assertIn("_fa = (uint32_t)(MEM32(esi)) & 0xFFFFFFFFu;", c)

    def test_memory_destination_snapshots_the_value_written(self):
        c = self.lift("0106C3", {"rmw-snapshot"})
        self.assertIn("{ uint32_t _r = (uint32_t)(MEM32(esi) + eax); MEM32(esi) = _r;"
                      " _fa = _r & 0xFFFFFFFFu;", c)
        self.assertNotIn("(MEM32(esi)) & 0xFFFFFFFFu", c)

    def test_narrow_destination_masks_like_a_read_back(self):
        c = self.lift("D03EC3", {"rmw-snapshot"})      # sar byte ptr [esi], 1
        self.assertIn("_fa = _r & 0xFFu; _fas = (int32_t)(int8_t)(_fa);", c)

    def test_inc_dec_and_neg(self):
        c = self.lift("FF06C3", {"rmw-snapshot"})      # inc dword ptr [esi]
        self.assertIn("{ uint32_t _r = (uint32_t)(MEM32(esi) + 1); MEM32(esi) = _r;", c)
        self.assertIn("_fb = (_fa == 0x80000000u);", c)
        c = self.lift("F71EC3", {"rmw-snapshot"})      # neg dword ptr [esi]
        self.assertIn("uint32_t _r = (uint32_t)((uint32_t)(-(int32_t)MEM32(esi)))", c)

    def test_register_destination_is_untouched(self):
        self.assertEqual(self.lift("01D8C3", {"rmw-snapshot"}),   # add eax, ebx
                         self.lift("01D8C3"))


class FcmpFloatTest(_Lift):
    def test_comiss_snapshots_are_float(self):
        code = "0F2FC17700C3"                          # comiss xmm0, xmm1; ja; ret
        self.assertIn("double _fca = 0.0, _fcb = 0.0;", self.lift(code))
        self.assertIn("float _fca = 0.0f, _fcb = 0.0f;", self.lift(code, {"fcmp-float"}))

    def test_comisd_keeps_double(self):
        c = self.lift("660F2FC17700C3", {"fcmp-float"})   # comisd
        self.assertIn("double _fca = 0.0, _fcb = 0.0;", c)


class XmmIntrinsicsTest(unittest.TestCase):
    """Both builds of the packed helpers, lane for lane, on edge values."""

    HELPERS = ["XMM_ADD", "XMM_SUB", "XMM_MUL", "XMM_DIV", "XMM_MIN", "XMM_MAX",
               "XMM_AND", "XMM_OR", "XMM_XOR", "XMM_ANDN", "XMM_CMP_EQ",
               "XMM_CMP_LT", "XMM_CMP_LE", "XMM_CMP_NEQ", "XMM_UNPACK_LOW",
               "XMM_UNPACK_HIGH"]

    def test_defines(self):
        self.assertEqual(perf_defines({"xmm-intrinsics"}),
                         ["#define RECOMP_XMM_INTRINSICS 1"])
        self.assertEqual(perf_defines(set()), [])

    def test_both_builds_agree(self):
        cc = shutil.which("gcc") or shutil.which("clang")
        if not cc:
            self.skipTest("no C compiler")
        tmp = tempfile.mkdtemp()
        try:
            for side, define in (("lane", ""), ("sse", "#define RECOMP_XMM_INTRINSICS 1\n")):
                body = [define, '#include "recomp_types.h"']
                for i, h in enumerate(self.HELPERS):
                    body.append(f"RecompXmm {side}_{i}(RecompXmm a, RecompXmm b)"
                                f" {{ return {h}(a, b); }}")
                for imm in (0x00, 0x1B, 0x4E, 0xE4, 0xFF):
                    body.append(f"RecompXmm {side}_shuf{imm}(RecompXmm a, RecompXmm b)"
                                f" {{ return XMM_SHUFFLE(a, b, 0x{imm:X}); }}")
                body.append(f"uint32_t {side}_mask(RecompXmm a) {{ return XMM_MOVEMASK(a); }}")
                body.append(f"RecompXmm {side}_load(uint32_t a) {{ return XMM_MEM(a); }}")
                body.append(f"void {side}_store(uint32_t a, RecompXmm v) {{ XMM_STORE(a, v); }}")
                with open(os.path.join(tmp, side + ".c"), "w") as f:
                    f.write("\n".join(body) + "\n")
            n = len(self.HELPERS)
            main = ['#include <stdio.h>', '#include <string.h>', '#include <stdint.h>',
                    '#include <stddef.h>',
                    'typedef union { float f[4]; double d[2]; uint32_t u[4]; int32_t i[4]; uint64_t q[2]; } X;',
                    'ptrdiff_t g_xbox_mem_offset;']
            for side in ("lane", "sse"):
                for i in range(n):
                    main.append(f"X {side}_{i}(X, X);")
                for imm in (0x00, 0x1B, 0x4E, 0xE4, 0xFF):
                    main.append(f"X {side}_shuf{imm}(X, X);")
                main.append(f"uint32_t {side}_mask(X); X {side}_load(uint32_t);"
                            f" void {side}_store(uint32_t, X);")
            specials = [0x00000000, 0x80000000, 0x3F800000, 0xBF800000, 0x7F800000,
                        0xFF800000, 0x7FC00000, 0x7FA00001, 0x00000001, 0x80000001,
                        0x7F7FFFFF, 0x40490FDB, 0xC2C80000, 0x00800000, 0x3EAAAAAB,
                        0x5A5A5A5A]
            main.append("static const uint32_t S[] = {%s};" %
                        ", ".join(f"0x{v:08X}u" for v in specials))
            main.append(r'''
static int same(X a, X b) { return memcmp(&a, &b, 16) == 0; }
int main(void) {
    static uint8_t mem[64];
    int bad = 0, i, j, k;
    g_xbox_mem_offset = (ptrdiff_t)mem;
    for (i = 0; i < 16; i++) for (j = 0; j < 16; j++) {
        X a, b;
        for (k = 0; k < 4; k++) { a.u[k] = S[(i + k * 5) % 16]; b.u[k] = S[(j + k * 3) % 16]; }
#define CHK(L, R) if (!same(L, R)) { printf("%s a=%d b=%d\n", #L, i, j); bad++; }
''' + "\n".join(f"        CHK(lane_{t}(a, b), sse_{t}(a, b));" for t in
                [str(x) for x in range(n)] + [f"shuf{imm}" for imm in (0x00, 0x1B, 0x4E, 0xE4, 0xFF)]) + r'''
        if (lane_mask(a) != sse_mask(a)) { printf("mask %d\n", i); bad++; }
        memcpy(mem + 3, &a, 16);
        CHK(lane_load(3), sse_load(3));
        sse_store(20, b); { X r = lane_load(20); CHK(r, b); }
    }
    printf("%d\n", bad);
    return bad != 0;
}''')
            with open(os.path.join(tmp, "main.c"), "w") as f:
                f.write("\n".join(main) + "\n")
            exe = os.path.join(tmp, "xmm")
            r = subprocess.run([cc, "-O2", "-w", "-I", RUNTIME, "-o", exe,
                                os.path.join(tmp, "lane.c"), os.path.join(tmp, "sse.c"),
                                os.path.join(tmp, "main.c"), "-lm"],
                               capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stderr[-3000:])
            r = subprocess.run([exe], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stdout[-3000:])
        finally:
            shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    unittest.main()
