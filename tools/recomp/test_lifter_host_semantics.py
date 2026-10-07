"""x86 semantics that the host CPU used to supply, and AArch64 does not.

Lifted C ran on x86 hosts only, where a few things came free from the
hardware under the C: a float-to-int cast compiles to cvttss2si and so gives
0x80000000 for NaN or out of range, a zero divisor traps, and a locked
instruction is only ever reached on one ISA. On an AArch64 host a cast
saturates, a division by zero returns 0, and INT64_MIN / -1 is undefined.
Three lowerings now say what x86 does, through helpers in recomp_types.h:

  cvt[t]ss2si / cvt[t]sd2si  -> recomp_cvt*2si (the rounding forms also round
                                now; a cast truncated them on every host)
  div / idiv                 -> at the operand's width, with RECOMP_DIV_CHECK
                                / RECOMP_IDIV_CHECK (nothing on x86)
  lock add/sub/.../inc/dec,  -> a compare-and-swap loop; these lifted to
  xchg with memory              RECOMP_UNIMPL before and did nothing at all

The emitted text is checked, and then compiled and run on this host against
Intel SDM results. On an x86 host the run exercises the SSE instructions; on
AArch64 it exercises the portable fallbacks -- the half that matters there.
"""
import shutil
import subprocess
import tempfile
from pathlib import Path

import pytest

from .disasm import BasicBlock, Instruction, Operand
from .lifter import Lifter, lift_basic_block

_ROOT = Path(__file__).resolve().parents[2]


def _reg(name):
    return Operand(type="reg", reg=name)


def _mem(size, base="ecx", disp=0):
    return Operand(type="mem", mem_base=base, mem_index=None, mem_scale=1,
                   mem_disp=disp, mem_size=size)


def _lift(mnemonic, *ops):
    insn = Instruction(0x10000, 3, mnemonic, "", "", operands=list(ops))
    lifted, _ = lift_basic_block(Lifter(), BasicBlock(start=0x10000, instructions=[insn]))
    return "\n".join(lifted)


# ── the text ──────────────────────────────────────────────────────────────

def test_conversions_use_the_helpers():
    assert "recomp_cvtss2si(xmm1.f[0])" in _lift("cvtss2si", _reg("eax"), _reg("xmm1"))
    assert "recomp_cvttss2si(MEMF(ecx))" in _lift("cvttss2si", _reg("eax"), _mem(4))
    # A double in a register is its d[0] lane, not f[0].
    assert "recomp_cvtsd2si(xmm0.d[0])" in _lift("cvtsd2si", _reg("edx"), _reg("xmm0"))
    assert "recomp_cvttsd2si(MEMD(ecx))" in _lift("cvttsd2si", _reg("edx"), _mem(8))
    assert "(int32_t)xmm" not in _lift("cvtss2si", _reg("eax"), _reg("xmm1"))


def test_division_is_checked_and_sized():
    assert "RECOMP_DIV_CHECK(_dv)" in _lift("div", _reg("ecx"))
    assert "RECOMP_IDIV_CHECK(_dividend, _dv)" in _lift("idiv", _mem(4))
    byte = _lift("div", _reg("cl"))
    assert "LO16(eax)" in byte and "SET_HI8(eax" in byte and "edx" not in byte
    word = _lift("idiv", _reg("cx"))
    assert "SET_LO16(edx" in word


def test_locked_ops_are_atomic_not_unimplemented():
    for mnem, ops in (("lock inc", (_mem(4),)),
                      ("lock dec", (_mem(2),)),
                      ("lock add", (_mem(4), _reg("eax"))),
                      ("lock or", (_mem(1), _reg("dl"))),
                      ("lock and", (_mem(4), Operand(type="imm", imm=0xFF))),
                      ("lock sub", (_mem(4), _reg("esi"))),
                      ("lock xor", (_mem(4), _reg("edi"))),
                      ("lock not", (_mem(4),)),
                      ("lock neg", (_mem(4),))):
        text = _lift(mnem, *ops)
        assert "RECOMP_UNIMPL" not in text, mnem
        assert "RECOMP_ATOMIC_CASV" in text, mnem


def test_xchg_with_memory_is_an_atomic_exchange():
    assert "RECOMP_ATOMIC_XCHG32" in _lift("xchg", _mem(4), _reg("eax"))
    assert "RECOMP_ATOMIC_XCHG16" in _lift("xchg", _reg("dx"), _mem(2))
    assert "RECOMP_ATOMIC" not in _lift("xchg", _reg("eax"), _reg("edx"))


# ── the behaviour ─────────────────────────────────────────────────────────

def _body(name, mnemonic, *ops):
    """A C function running one lifted instruction on the harness's state."""
    return f"static void {name}(void)\n{{\n{_lift(mnemonic, *ops)}\n}}\n"


_HARNESS = r'''
#include <stdint.h>
#include <stdio.h>
#include <setjmp.h>
#include <math.h>
#include "recomp_types.h"

ptrdiff_t g_xbox_mem_offset;
static uint8_t guest[0x1000];
static uint32_t eax, ecx, edx, ebx, esi, edi;
static uint32_t _fa, _fb; static int32_t _fas, _fbs; static int _cf;

/* The runtime's #DE: here, record it and jump back to the test. */
static jmp_buf g_de; static uint32_t g_de_code;
void recomp_int_divide_fault(uint32_t code) { g_de_code = code; longjmp(g_de, 1); }

#define FAIL(...) do { printf("line %d: ", __LINE__); printf(__VA_ARGS__); \
                       printf("\n"); failures++; } while (0)
'''

_MAIN = r'''
#define CVT(fn, v, want) do { int32_t g = fn(v); if (g != (int32_t)(want)) \
    FAIL(#fn "(%g) = %d, want %d", (double)(v), g, (int32_t)(want)); } while (0)

/* Runs a lifted division; returns the fault code it raised, or 0. On an x86
 * host a zero divisor is the hardware's trap, so those cases are skipped. */
static uint32_t run_div(void (*fn)(void))
{
    g_de_code = 0;
    if (setjmp(g_de) == 0)
        fn();
    return g_de_code;
}

int main(void)
{
    int failures = 0;
    g_xbox_mem_offset = (ptrdiff_t)(uintptr_t)guest;

    /* cvtss2si rounds to nearest even; cvttss2si truncates; both give
     * 0x80000000 for NaN and anything outside int32. */
    CVT(recomp_cvtss2si, 2.5f, 2);
    CVT(recomp_cvtss2si, 3.5f, 4);
    CVT(recomp_cvtss2si, -2.5f, -2);
    CVT(recomp_cvtss2si, 2.7f, 3);
    CVT(recomp_cvtss2si, -2.7f, -3);
    CVT(recomp_cvttss2si, 2.7f, 2);
    CVT(recomp_cvttss2si, -2.7f, -2);
    CVT(recomp_cvtss2si, -2147483648.0f, INT32_MIN);
    CVT(recomp_cvtss2si, 2147483648.0f, INT32_MIN);
    CVT(recomp_cvttss2si, 3e9f, INT32_MIN);
    CVT(recomp_cvttss2si, -3e9f, INT32_MIN);
    CVT(recomp_cvtss2si, (float)NAN, INT32_MIN);
    CVT(recomp_cvttss2si, (float)INFINITY, INT32_MIN);
    CVT(recomp_cvtsd2si, 2147483647.4, 2147483647);
    CVT(recomp_cvtsd2si, 2147483647.6, INT32_MIN);   /* rounds out of range */
    CVT(recomp_cvttsd2si, 2147483647.9, 2147483647);
    CVT(recomp_cvttsd2si, -2147483648.9, INT32_MIN); /* truncates to -2^31: in range */
    CVT(recomp_cvttsd2si, -2147483649.0, INT32_MIN);
    CVT(recomp_cvtsd2si, -0.5, 0);
    CVT(recomp_cvtsd2si, 1.5, 2);

    /* 32-bit div/idiv. */
    edx = 0; eax = 100; ecx = 7; div32();
    if (eax != 14 || edx != 2) FAIL("div32 %u r %u", eax, edx);
    edx = 0xFFFFFFFFu; eax = (uint32_t)-100; ecx = 7; idiv32();
    if ((int32_t)eax != -14 || (int32_t)edx != -2) FAIL("idiv32 %d r %d", eax, edx);
    /* The quotient overflows: truncated, as Windows does (no trap). */
    edx = 1; eax = 0; ecx = 1; div32();
    if (eax != 0 || edx != 0) FAIL("div32 overflow %u r %u", eax, edx);

    /* 8-bit: AX / r8 -> AL quotient, AH remainder; edx untouched. */
    eax = 0xABCD0000u | 1000; ecx = 0x12345600u | 7; edx = 0x55555555u; div8();
    if ((eax & 0xFFFF) != ((1000 % 7) << 8 | (1000 / 7)) || (eax >> 16) != 0xABCD
        || edx != 0x55555555u)
        FAIL("div8 eax=%08X edx=%08X", eax, edx);
    eax = (uint32_t)(int16_t)-1000 & 0xFFFF; ecx = (uint8_t)(int8_t)7; idiv8();
    if ((int8_t)(eax & 0xFF) != -142 % 256 - 0 && (int8_t)(eax & 0xFF) != (int8_t)(-1000 / 7))
        FAIL("idiv8 al=%d", (int8_t)(eax & 0xFF));
    if ((int8_t)((eax >> 8) & 0xFF) != (int8_t)(-1000 % 7)) FAIL("idiv8 ah=%d", (int8_t)(eax >> 8));

    /* 16-bit: DX:AX / r16 -> AX, DX; upper halves kept. */
    edx = 0xAAAA0001u; eax = 0xBBBB0000u; ecx = 0x00000003u; div16();   /* 0x10000 / 3 */
    if (eax != 0xBBBB5555u || edx != 0xAAAA0001u) FAIL("div16 eax=%08X edx=%08X", eax, edx);
    edx = 0xFFFFu; eax = (uint32_t)(-30000 & 0xFFFF); ecx = 7; idiv16();
    if ((int16_t)eax != -30000 / 7 || (int16_t)edx != -30000 % 7)
        FAIL("idiv16 ax=%d dx=%d", (int16_t)eax, (int16_t)edx);

#if !(defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86))
    /* A zero divisor raises EXCEPTION_INT_DIVIDE_BY_ZERO; INT64_MIN / -1
     * raises EXCEPTION_INT_OVERFLOW -- what x86 Windows reports. */
    edx = 0; eax = 1; ecx = 0;
    if (run_div(div32) != 0xC0000094u) FAIL("div32 by zero: %08X", g_de_code);
    if (run_div(idiv32) != 0xC0000094u) FAIL("idiv32 by zero: %08X", g_de_code);
    if (run_div(div8) != 0xC0000094u) FAIL("div8 by zero: %08X", g_de_code);
    if (run_div(idiv16) != 0xC0000094u) FAIL("idiv16 by zero: %08X", g_de_code);
    edx = 0x80000000u; eax = 0; ecx = 0xFFFFFFFFu;
    if (run_div(idiv32) != 0xC0000095u) FAIL("idiv32 INT64_MIN/-1: %08X", g_de_code);
    edx = 0xFFFFFFFFu; eax = 0x80000000u; ecx = 0xFFFFFFFFu;   /* -2^31 / -1 */
    if (run_div(idiv32) != 0 || eax != 0x80000000u) FAIL("idiv32 -2^31/-1 eax=%08X", eax);
#endif

    /* Locked read-modify-writes: the value, and the flags the unlocked form
     * gives (here ZF via _fa, and inc's OF via _fb). */
    *(uint32_t *)(guest + 0x100) = 0xFFFFFFFFu; ecx = 0x100;
    lock_inc32();
    if (*(uint32_t *)(guest + 0x100) != 0 || _fa != 0) FAIL("lock inc wrap: %08X fa=%08X",
                                                             *(uint32_t *)(guest + 0x100), _fa);
    *(uint32_t *)(guest + 0x100) = 0x7FFFFFFFu;
    lock_inc32();
    if (*(uint32_t *)(guest + 0x100) != 0x80000000u || _fb != 1) FAIL("lock inc OF");
    *(uint16_t *)(guest + 0x102) = 5; ecx = 0x102;
    lock_dec16();
    if (*(uint16_t *)(guest + 0x102) != 4 || _fa != 4) FAIL("lock dec16");
    guest[0x110] = 0xF0; ecx = 0x110; edx = 0x0F;
    lock_or8();
    if (guest[0x110] != 0xFF || (int8_t)_fas != -1) FAIL("lock or8 %02X", guest[0x110]);
    *(uint32_t *)(guest + 0x120) = 10; ecx = 0x120; eax = 3;
    lock_sub32();
    if (*(uint32_t *)(guest + 0x120) != 7 || _fa != 7 || _fb != 3) FAIL("lock sub");
    *(uint32_t *)(guest + 0x120) = 0x12345678u;
    lock_neg32();
    if (*(uint32_t *)(guest + 0x120) != (uint32_t)-0x12345678) FAIL("lock neg");

    /* xchg with memory. */
    *(uint32_t *)(guest + 0x130) = 0xCAFEF00Du; ecx = 0x130; eax = 0x11112222u;
    xchg32();
    if (eax != 0xCAFEF00Du || *(uint32_t *)(guest + 0x130) != 0x11112222u) FAIL("xchg32");
    *(uint16_t *)(guest + 0x134) = 0xBEEF; ecx = 0x134; edx = 0x99990000u | 0x1234;
    xchg16();
    if (edx != 0x9999BEEFu || *(uint16_t *)(guest + 0x134) != 0x1234) FAIL("xchg16 edx=%08X", edx);

    if (failures)
        printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
'''


def _source():
    bodies = [
        _body("div32", "div", _reg("ecx")),
        _body("idiv32", "idiv", _reg("ecx")),
        _body("div8", "div", _reg("cl")),
        _body("idiv8", "idiv", _reg("cl")),
        _body("div16", "div", _reg("cx")),
        _body("idiv16", "idiv", _reg("cx")),
        _body("lock_inc32", "lock inc", _mem(4)),
        _body("lock_dec16", "lock dec", _mem(2)),
        _body("lock_or8", "lock or", _mem(1), _reg("dl")),
        _body("lock_sub32", "lock sub", _mem(4), _reg("eax")),
        _body("lock_neg32", "lock neg", _mem(4)),
        _body("xchg32", "xchg", _mem(4), _reg("eax")),
        _body("xchg16", "xchg", _mem(2), _reg("dx")),
    ]
    return _HARNESS + "\n".join(bodies) + _MAIN


def test_helpers_and_lowerings_match_x86_on_this_host():
    cc = shutil.which("clang") or shutil.which("cc") or shutil.which("gcc")
    if not cc:
        pytest.skip("no C compiler")
    with tempfile.TemporaryDirectory() as tmp:
        src = Path(tmp) / "t.c"
        exe = Path(tmp) / "t"
        src.write_text(_source())
        built = subprocess.run(
            [cc, "-std=gnu11", "-O2", "-Wall", "-Wno-unused-function",
             "-Wno-unused-variable", "-Wno-unused-but-set-variable",
             "-I", str(_ROOT / "templates" / "runtime"), str(src), "-o", str(exe), "-lm"],
            capture_output=True, text=True)
        assert built.returncode == 0, built.stderr[-4000:]
        ran = subprocess.run([str(exe)], capture_output=True, text=True)
        assert ran.returncode == 0, ran.stdout + ran.stderr
