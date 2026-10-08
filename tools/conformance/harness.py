"""C generation for the conformance run: the native side, and the comparison.

Newlines inside generated C string literals are written as @NL@ and substituted
at the end, so the escape survives however this file is edited.
"""

MARK = ["nop", "nop", "nop"]
_MARK_BYTES = "90 90 90"

# Round to nearest, all exceptions masked, and precision control = double
# (53-bit) rather than the x87 default of extended (64-bit). Our model holds
# the stack as C doubles, so at extended precision the hardware would carry
# more bits than the model can and add/sub/mul/div/sqrt would disagree in the
# last place for reasons that have nothing to do with lifting. At PC=53 those
# five are correctly rounded to double on both sides and must match exactly;
# only the transcendentals (hardware polynomial vs libm) still diverge.
FP_CONTROL_WORD = 0x027F

_SETUP = {
    "gpr": """        mov eax, g_in_a
        mov ecx, g_in_b
        xor edx, edx""",
    "fpu": """        finit
        fldcw word ptr g_fp_cw
        mov eax, g_scratch_ptr
        xor ecx, ecx
        xor edx, edx""",
    "sse": """        mov eax, g_scratch_ptr
        xor ecx, ecx
        xor edx, edx
        xorps xmm0, xmm0
        xorps xmm1, xmm1
        xorps xmm2, xmm2
        xorps xmm3, xmm3
        xorps xmm4, xmm4
        xorps xmm5, xmm5
        xorps xmm6, xmm6
        xorps xmm7, xmm7""",
}

# After the snippet: eax first (a case may end with `fnstsw ax`), then the
# state. The x87 stack is drained with eight fstp's -- reading past the live
# entries is harmless because the C side only compares `depth` of them, and
# depth comes from the status word's TOP field before any of this runs.
_CAPTURE = {
    "gpr": """        mov g_out_eax, eax""",
    "fpu": """        mov g_out_eax, eax
        fnstsw word ptr g_out_sw
        fstp qword ptr g_out_st[0]
        fstp qword ptr g_out_st[8]
        fstp qword ptr g_out_st[16]
        fstp qword ptr g_out_st[24]
        fstp qword ptr g_out_st[32]
        fstp qword ptr g_out_st[40]
        fstp qword ptr g_out_st[48]
        fstp qword ptr g_out_st[56]""",
    "sse": """        mov g_out_eax, eax
        movups xmmword ptr g_out_xmm[0], xmm0
        movups xmmword ptr g_out_xmm[16], xmm1
        movups xmmword ptr g_out_xmm[32], xmm2
        movups xmmword ptr g_out_xmm[48], xmm3
        movups xmmword ptr g_out_xmm[64], xmm4
        movups xmmword ptr g_out_xmm[80], xmm5
        movups xmmword ptr g_out_xmm[96], xmm6
        movups xmmword ptr g_out_xmm[112], xmm7""",
}

_SHARED_STATE = """unsigned int g_in_a, g_in_b;
unsigned int g_out_eax;
unsigned short g_out_sw;
unsigned short g_fp_cw = 0x027F;
double g_out_st[8];
unsigned char g_out_xmm[128];
unsigned char *g_scratch_ptr;
"""


def native_source(cases):
    out = ["/* generated -- native side: the real instructions on the real CPU */",
           _SHARED_STATE]
    for c in cases:
        body = "\n".join(f"        {i}" for i in MARK + c["asm"] + MARK)
        out.append(f"""void nat_{c['name']}(void) {{
    __asm {{
{_SETUP[c['kind']]}
{body}
{_CAPTURE[c['kind']]}
    }}
}}""")
    return "\n".join(out)


_PREAMBLE = """/* generated -- both sides, same inputs, compared */
#define RECOMP_GENERATED_CODE 1
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "recomp_types.h"

/* The guest register file (recomp_cpu.h): one thread-local struct. */
RECOMP_TLS struct recomp_cpu g_cpu = { .r_fp_control_word = 0x027F, .r_fp_cc = 0x4000 };
RECOMP_TLS RecompXmm g_xmm0,g_xmm1,g_xmm2,g_xmm3,g_xmm4,g_xmm5,g_xmm6,g_xmm7;
RECOMP_TLS RecompMmx g_mm0,g_mm1,g_mm2,g_mm3,g_mm4,g_mm5,g_mm6,g_mm7;
volatile uint32_t g_icall_trace[16]; volatile uint32_t g_icall_trace_idx;
volatile uint64_t g_icall_count;
ptrdiff_t g_xbox_mem_offset;
void recomp_icall_fail_log(uint32_t va) { (void)va; }
void recomp_unimpl(const char *text, uint32_t va) { (void)text; (void)va; }

#if defined(_MSC_VER)
#define CONF_ALIGN16 __declspec(align(16))
#else
#define CONF_ALIGN16 __attribute__((aligned(16)))
#endif
#define CONF_STACK_BYTES (64 * 1024)

#ifdef CONF_GOLDEN
/* Golden mode: no native side in this process -- its results were recorded
   on an x86 machine (--record) and are loaded instead. The state the native
   side would have defined lives here. */
unsigned int g_in_a, g_in_b;
unsigned int g_out_eax;
unsigned short g_out_sw;
double g_out_st[8];
unsigned char g_out_xmm[128];
unsigned char *g_scratch_ptr;
/* One block, reached through g_xbox_mem_offset so that the guest address of
   the scratch buffer is the one the recording saw: a 64-bit host cannot put
   it below 4 GB, and a case whose eax ends up holding the pointer must still
   compare equal. */
static CONF_ALIGN16 unsigned char g_arena[0x1000 + CONF_STACK_BYTES];
#define g_scratch     g_arena
#define g_guest_stack (g_arena + 0x1000)
#else
extern unsigned int g_in_a, g_in_b;
extern unsigned int g_out_eax;
extern unsigned short g_out_sw;
extern double g_out_st[8];
extern unsigned char g_out_xmm[128];
extern unsigned char *g_scratch_ptr;

/* Guest addresses are host addresses here (g_xbox_mem_offset stays 0), so a
   memory operand reads the same bytes on both sides. 16-byte aligned for the
   aligned SSE moves. */
static CONF_ALIGN16 unsigned char g_scratch[64];
static unsigned char g_guest_stack[CONF_STACK_BYTES];
#endif
/* A host pointer as the guest sees it: itself, unless golden mode moved it. */
#define GUEST_VA(p) ((uint32_t)((uintptr_t)(p) - (uintptr_t)g_xbox_mem_offset))

/* A lifted division by zero on a host whose own division does not trap
   (recomp_types.h): the native side would have faulted, so say so and stop. */
#if !(defined(_M_IX86) || defined(_M_X64) || defined(__i386__) || defined(__x86_64__))
#include <stdlib.h>
void recomp_int_divide_fault(uint32_t code)
{
    printf("lifted division raised 0x%08X@NL@", code);
    exit(3);
}
#endif

/* What the lifted run produced, in the same shape as the native capture. */
static unsigned int  l_eax;
static int           l_depth, n_depth;
static double        l_st[8], n_st[8];
static unsigned char l_xmm[128];
"""

_LIFTED_PROLOGUE = {
    "gpr": """    g_eax = g_in_a; g_ecx = g_in_b; g_edx = 0;""",
    "fpu": """    g_fp_top = 0; g_fp_control_word = 0x027Fu; g_fp_cmp = 0; g_fp_cc = 0x4000;
@FPMACROS@
    memset(g_fp_stack, 0, sizeof(g_fp_stack));
    g_eax = GUEST_VA(g_scratch); g_ecx = 0; g_edx = 0;""",
    "sse": """    memset(&g_xmm0, 0, sizeof(g_xmm0)); memset(&g_xmm1, 0, sizeof(g_xmm1));
    memset(&g_xmm2, 0, sizeof(g_xmm2)); memset(&g_xmm3, 0, sizeof(g_xmm3));
    memset(&g_xmm4, 0, sizeof(g_xmm4)); memset(&g_xmm5, 0, sizeof(g_xmm5));
    memset(&g_xmm6, 0, sizeof(g_xmm6)); memset(&g_xmm7, 0, sizeof(g_xmm7));
    g_eax = GUEST_VA(g_scratch); g_ecx = 0; g_edx = 0;""",
}

# The model's TOP moves the same way the hardware's does: fp_push decrements it
# modulo 8, so with an empty stack at 0 the depth is simply -TOP mod 8. st(i)
# is then the slot i above TOP.
_LIFTED_EPILOGUE = {
    "gpr": """    l_eax = g_eax;""",
    "fpu": """@FPUNDEFS@
    l_eax = g_eax;
    l_depth = (8 - (g_fp_top & 7)) & 7;
    { int _i; for (_i = 0; _i < 8; _i++)
        l_st[_i] = g_fp_stack[(g_fp_top + _i) & 7]; }""",
    "sse": """    l_eax = g_eax;
    memcpy(l_xmm +   0, &g_xmm0, 16); memcpy(l_xmm +  16, &g_xmm1, 16);
    memcpy(l_xmm +  32, &g_xmm2, 16); memcpy(l_xmm +  48, &g_xmm3, 16);
    memcpy(l_xmm +  64, &g_xmm4, 16); memcpy(l_xmm +  80, &g_xmm5, 16);
    memcpy(l_xmm +  96, &g_xmm6, 16); memcpy(l_xmm + 112, &g_xmm7, 16);""",
}


_COMPARE = """
static int g_total, g_fail;

/* Two doubles agree if they are bit-identical, or both NaN, or -- only where
   the case allows it -- within a relative tolerance. Bit-identical is the
   default on purpose: it keeps -0.0 distinct from +0.0, which is exactly the
   kind of difference a lifted min/max tie-break gets wrong. */
static int same_double(double a, double b, double tol) {
    if (memcmp(&a, &b, sizeof a) == 0) return 1;
    if (isnan(a) && isnan(b)) return 1;
    if (tol > 0.0 && !isnan(a) && !isnan(b)) {
        double d = fabs(a - b), s = fabs(a) > fabs(b) ? fabs(a) : fabs(b);
        return d <= tol * (s > 1.0 ? s : 1.0);
    }
    return 0;
}

static void report(const char *name, const char *why, int *shown) {
    if (!*shown) printf("FAIL %s  (%s)@NL@", name, why);
    (*shown)++;
}

static void cmp_gpr(const char *name, const char *why, int *shown, int vec) {
    if (g_out_eax == l_eax) return;
    report(name, why, shown);
    if (*shown <= 3)
        printf("       vec %-3d eax  native=%08X  lifted=%08X@NL@",
               vec, g_out_eax, l_eax);
    g_fail++;
}

static void cmp_fpu(const char *name, const char *why, int *shown, int vec,
                    double tol) {
    int i, bad = 0;
    if (g_out_eax != l_eax) bad = 1;
    if (n_depth != l_depth) bad = 2;
    if (!bad)
        for (i = 0; i < n_depth; i++)
            if (!same_double(n_st[i], l_st[i], tol)) { bad = 3 + i; break; }
    if (!bad) return;
    report(name, why, shown);
    if (*shown <= 3) {
        if (bad == 1)
            printf("       vec %-3d eax  native=%08X  lifted=%08X@NL@",
                   vec, g_out_eax, l_eax);
        else if (bad == 2)
            printf("       vec %-3d STACK DEPTH native=%d  lifted=%d@NL@",
                   vec, n_depth, l_depth);
        else {
            i = bad - 3;
            printf("       vec %-3d st(%d)  native=%.17g  lifted=%.17g@NL@",
                   vec, i, n_st[i], l_st[i]);
        }
    }
    g_fail++;
}

static void cmp_sse(const char *name, const char *why, int *shown, int vec) {
    int r, lane, bad = -1;
    if (g_out_eax != l_eax) {
        report(name, why, shown);
        if (*shown <= 3)
            printf("       vec %-3d eax  native=%08X  lifted=%08X@NL@",
                   vec, g_out_eax, l_eax);
        g_fail++;
        return;
    }
    if (memcmp(g_out_xmm, l_xmm, sizeof l_xmm) == 0) return;
    for (r = 0; r < 8 && bad < 0; r++)
        for (lane = 0; lane < 4; lane++)
            if (memcmp(g_out_xmm + r * 16 + lane * 4,
                       l_xmm + r * 16 + lane * 4, 4) != 0) {
                bad = r * 4 + lane; break;
            }
    report(name, why, shown);
    if (*shown <= 3 && bad >= 0) {
        r = bad / 4; lane = bad % 4;
        {   unsigned int nb, lb; float nf, lf;
            memcpy(&nb, g_out_xmm + r * 16 + lane * 4, 4);
            memcpy(&lb, l_xmm     + r * 16 + lane * 4, 4);
            memcpy(&nf, &nb, 4); memcpy(&lf, &lb, 4);
            printf("       vec %-3d xmm%d lane %d  native=%08X (%g)  "
                   "lifted=%08X (%g)@NL@", vec, r, lane, nb, nf, lb, lf);
        }
    }
    g_fail++;
}
"""


# Golden values: the native side's results, recorded on an x86 machine with
# MSVC (`--record`) so the lifted side can be checked anywhere (`--golden`),
# including hosts that cannot run x86 at all. A record is
#   [eax, status word, x87 stack as 64 bytes of hex, xmm0-7 as 128 bytes of hex]
# per input vector, and the recording also notes the scratch buffer's address,
# which golden mode reproduces as a guest address.
_GOLD_TYPES = """
typedef struct {
    unsigned int   eax;
    unsigned short sw;
    unsigned char  st[64];
    unsigned char  xmm[128];
} conf_gold;

static void gold_load(const conf_gold *g)
{
    g_out_eax = g->eax;
    g_out_sw = g->sw;
    memcpy(g_out_st, g->st, sizeof g->st);
    memcpy(g_out_xmm, g->xmm, sizeof g->xmm);
}
"""

_GOLD_DUMP = """
static void gold_dump(const char *name, int vec)
{
    const unsigned char *st = (const unsigned char *)g_out_st;
    int i;
    printf("@GOLD %s %d %08X %04X ", name, vec, g_out_eax, (unsigned)g_out_sw);
    for (i = 0; i < 64; i++) printf("%02X", st[i]);
    printf(" ");
    for (i = 0; i < 128; i++) printf("%02X", g_out_xmm[i]);
    printf("@NL@");
}
"""


def _c_bytes(hexstr):
    return "{" + ",".join(f"0x{hexstr[i:i + 2]}" for i in range(0, len(hexstr), 2)) + "}"


def harness_source(prepared, why_of, tol_of, record=False, golden=None):
    """prepared: list of (name, kind, lifted_lines, inputs).

    record: also print every native result as an @GOLD line (--record).
    golden: {"scratch_va": int, "cases": {name: {"vectors": [...]}}} -- build
    with -DCONF_GOLDEN and no native object: the recorded results stand in
    for the native side (--golden)."""
    from tools.recomp.translator import FP_STACK_MACROS, FP_STACK_UNDEFS
    global _LIFTED_PROLOGUE, _LIFTED_EPILOGUE
    _LIFTED_PROLOGUE = dict(_LIFTED_PROLOGUE)
    _LIFTED_EPILOGUE = dict(_LIFTED_EPILOGUE)
    _LIFTED_PROLOGUE["fpu"] = _LIFTED_PROLOGUE["fpu"].replace(
        "@FPMACROS@", chr(10).join(FP_STACK_MACROS))
    _LIFTED_EPILOGUE["fpu"] = _LIFTED_EPILOGUE["fpu"].replace(
        "@FPUNDEFS@", chr(10).join(FP_STACK_UNDEFS))
    out = [("#define CONF_GOLDEN 1\n" if golden else "") + _PREAMBLE]
    if golden:
        out.append(_GOLD_TYPES)
        for name, _, _, inputs in prepared:
            vecs = golden["cases"][name]["vectors"]
            rows = ",\n    ".join(
                f"{{0x{v[0]:08X}u, 0x{v[1]:04X}u, {_c_bytes(v[2])}, {_c_bytes(v[3])}}}"
                for v in vecs)
            out.append(f"static const conf_gold gold_{name}[{len(vecs)}] = {{\n    {rows}\n}};")
    else:
        for name, _, _, _ in prepared:
            out.append(f"void nat_{name}(void);")
        if record:
            out.append(_GOLD_DUMP)
    out.append("")
    for name, kind, lines, _ in prepared:
        body = "\n".join(f"    {l}" for l in lines) or "    /* nothing */"
        out.append(f"""static void lif_{name}(void) {{
    uint32_t ebp = 0; int _cf = 0; int _flags = 0;
    uint32_t _fa = 0, _fb = 0; int32_t _fas = 0, _fbs = 0;
    (void)ebp; (void)_cf; (void)_flags;
    (void)_fa; (void)_fb; (void)_fas; (void)_fbs;
{_LIFTED_PROLOGUE[kind]}
    g_esp = GUEST_VA(g_guest_stack + CONF_STACK_BYTES / 2);
{body}
{_LIFTED_EPILOGUE[kind]}
}}""")
    out.append(_COMPARE)
    out.append("int main(void) {\n    int shown;")
    if golden:
        out.append(f"    g_xbox_mem_offset = (ptrdiff_t)((uintptr_t)g_arena"
                   f" - (uintptr_t)0x{golden['scratch_va']:08X}u);")
    out.append("    g_scratch_ptr = g_scratch;")
    if record:
        out.append('    printf("@GOLDBASE %08X@NL@", (unsigned)(uintptr_t)g_scratch);')
    for name, kind, _, inputs in prepared:
        out.append(f"    shown = 0;   /* {name} */")
        for vec, inp in enumerate(inputs):
            out.append(f"    {{ {_load_inputs(kind, inp)}")
            if golden:
                native = f"gold_load(&gold_{name}[{vec}]);"
            else:
                native = f"nat_{name}();" + (f' gold_dump("{name}", {vec});'
                                             if record else "")
            out.append(f"      {native}"
                       + ("\n      n_depth = (8 - ((g_out_sw >> 11) & 7)) & 7;"
                          "\n      memcpy(n_st, g_out_st, sizeof n_st);"
                          if kind == "fpu" else ""))
            out.append(f"      lif_{name}(); g_total++;")
            out.append(f"      cmp_{kind}(\"{name}\", \"{why_of[name]}\", "
                       f"&shown, {vec}"
                       + (f", {tol_of[name]!r}" if kind == "fpu" else "") + "); }")
        out.append('    if (shown > 3) printf("       ... and %d more@NL@", '
                   'shown - 3);')
    out.append('    printf("@NL@%d vectors, %d mismatches@NL@", g_total, g_fail);'
               "\n    return g_fail != 0;\n}")
    return "\n".join(out).replace("@NL@", chr(92) + "n")


def parse_gold_lines(stdout):
    """The @GOLD lines a --record run printed: (scratch_va, {name: [vectors]})."""
    base, cases = None, {}
    for line in stdout.splitlines():
        parts = line.split()
        if parts[:1] == ["@GOLDBASE"]:
            base = int(parts[1], 16)
        elif parts[:1] == ["@GOLD"] and len(parts) == 7:
            name, vec = parts[1], int(parts[2])
            row = [int(parts[3], 16), int(parts[4], 16), parts[5], parts[6]]
            vecs = cases.setdefault(name, [])
            if vec != len(vecs):
                raise RuntimeError(f"@GOLD {name}: vector {vec} out of order")
            vecs.append(row)
    return base, cases


def _load_inputs(kind, inp):
    if kind == "gpr":
        return f"g_in_a = 0x{inp[0]:08X}u; g_in_b = 0x{inp[1]:08X}u;"
    if kind == "fpu":
        return (f"memset(g_scratch, 0, sizeof g_scratch);"
                f" {{ double _a = {_fp(inp[0], False)},"
                f" _b = {_fp(inp[1], False)};"
                f" memcpy(g_scratch, &_a, 8); memcpy(g_scratch + 16, &_b, 8); }}")
    lanes_a = ", ".join(_fp(v, True) for v in inp[0])
    lanes_b = ", ".join(_fp(v, True) for v in inp[1])
    return (f"{{ float _a[4] = {{{lanes_a}}}, _b[4] = {{{lanes_b}}};"
            f" memcpy(g_scratch, _a, 16); memcpy(g_scratch + 16, _b, 16); }}")


def _fp(v, single):
    """A C literal that round-trips exactly, including NaN and the zeroes.

    NAN and INFINITY are macros, not numeric literals, so they take a cast
    rather than an `f` suffix -- `NANf` is an identifier and compiles to an
    int, which is how the signalling values silently became 0.
    """
    import math
    if math.isnan(v):
        return "(float)NAN" if single else "NAN"
    if math.isinf(v):
        sign = "" if v > 0 else "-"
        return f"({sign}(float)INFINITY)" if single else f"({sign}INFINITY)"
    return f"{v!r}f" if single else repr(v)
