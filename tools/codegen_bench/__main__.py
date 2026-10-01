"""Measure what the C compiler makes of lifted code, and what emitter changes buy.

    python3 -m tools.codegen_bench                 # every variant, gcc and clang
    python3 -m tools.codegen_bench --cc clang --variants base,locals
    python3 -m tools.codegen_bench --show matmul:m128   # print one variant's C

Each snippet in snippets/ is real x86-32 guest code. It is assembled with GNU
as, lifted by the real tools.recomp translator (the same FunctionTranslator the
pipeline uses), and compiled against the real templates/runtime/recomp_types.h.
A *variant* is either an edited copy of that header or a mechanical rewrite of
the lifted C that models a proposed emitter change. bench.c then times every
build over the same guest state and the run fails unless every build writes the
same guest memory, so a faster variant cannot also be a wrong one.

What it reports, per snippet: ns per call (best of --reps) and the number of
host instructions objdump finds in the lifted function. Prefer the time. The
instruction count is what docs/technical/lifted-code-quality-review.md was
asked to check, and it misleads on loops: the rep-stosd fast path adds static
instructions and runs twenty times faster.

Runs on Linux or macOS with gcc and/or clang, binutils (as, ld, objcopy,
objdump) and capstone. It needs no game files and no MSVC. MSVC numbers differ:
it does no type-based alias analysis, so it gains more from the register
changes and less from dropping volatile. See the review for both.
"""

import argparse
import os
import re
import shutil
import subprocess
import sys

from ..recomp import config
from ..recomp import perf_opts
from ..recomp.translator import FunctionTranslator, perf_defines

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
RUNTIME_HEADER = os.path.join(ROOT, "templates", "runtime", "recomp_types.h")

# name -> [(snippet file, guest VA, function)], bench.c case, iterations
BENCHES = {
    "clear":     ([("clear.s", 0x000A5230)], 0, 200000),
    "matmul":    ([("matmul.s", 0x000F7487)], 1, 2000000),
    "keysearch": ([("keysearch.s", 0x00011000)], 2, 2000000),
    "calls":     ([("caller.s", 0x00090200), ("callee.s", 0x00090100)], 3, 200000),
    "x87sum":    ([("x87sum.s", 0x00090000)], 4, 200000),
}

GPRS = ["eax", "ecx", "edx", "ebx", "esi", "edi", "esp"]


# ── lifting ──────────────────────────────────────────────────────────────

def assemble(path, va, workdir):
    """Assemble a snippet linked at its guest VA, so rel32 calls resolve."""
    stem = os.path.join(workdir, os.path.basename(path)[:-2])
    subprocess.check_call(["as", "--32", "-o", stem + ".o", path])
    subprocess.check_call(["ld", "-m", "elf_i386", f"-Ttext=0x{va:x}",
                           "-e", f"0x{va:x}", "-o", stem + ".elf", stem + ".o"])
    subprocess.check_call(["objcopy", "-O", "binary", "-j", ".text",
                           stem + ".elf", stem + ".bin"])
    with open(stem + ".bin", "rb") as f:
        return f.read()


def lift(path, va, workdir, opts=frozenset()):
    image = assemble(path, va, workdir)
    config._install(
        [config.Section(".text", va, len(image), 0, len(image), True)],
        entry_point=va, kernel_thunk_addr=va, origin="codegen_bench")
    db = {va: {"start": f"0x{va:08X}", "end": va + len(image),
               "_addr": va, "size": len(image)}}
    c = FunctionTranslator(image, db, perf_opts=opts).translate_function(va, db[va])
    if not c:
        raise SystemExit(f"{path}: the translator returned nothing")
    return c


# ── header variants ──────────────────────────────────────────────────────

def _must_replace(text, old, new, what, count=1):
    if text.count(old) < 1:
        raise SystemExit(f"{what}: expected text not found -- the runtime "
                         f"header or the lifter output changed; update "
                         f"tools/codegen_bench to match.\n  {old[:120]!r}")
    return text.replace(old, new) if count is None else text.replace(old, new, count)


def header_novol(h):
    """MEM* without volatile. Attribution only: NOT safe to ship, because the
    MMIO fault decoder needs one exact-width mov per access and guest spin
    loops need the reload. See the review, finding N4."""
    for w in ("uint8_t  ", "uint16_t ", "uint32_t ", "uint64_t ",
              "int8_t   ", "int16_t  ", "int32_t  ", "int64_t  ",
              "float    ", "double   "):
        h = _must_replace(h, f"(*(volatile {w}*)XBOX_PTR(addr))",
                          f"(*({w}*)XBOX_PTR(addr))", "novol")
    return h


def header_notls(h):
    """Guest registers as ordinary globals rather than thread-locals."""
    return _must_replace(h, "#  define RECOMP_TLS __thread",
                         "#  define RECOMP_TLS", "notls")


def header_tlsstruct(h):
    """One thread-local struct for the GPRs: one TLS base, fixed offsets."""
    return _must_replace(
        h,
        "extern RECOMP_TLS uint32_t g_eax, g_ecx, g_edx, g_esp;\n"
        "extern RECOMP_TLS uint32_t g_ebx, g_esi, g_edi;",
        "typedef struct RecompGuestRegs {\n"
        "    uint32_t eax, ecx, edx, esp, ebx, esi, edi;\n"
        "} RecompGuestRegs;\n"
        "extern RECOMP_TLS RecompGuestRegs g_regs;\n"
        + "".join(f"#define g_{r} g_regs.{r}\n" for r in GPRS),
        "tlsstruct")


def header_m128(h):
    """Packed-SSE helpers over SSE intrinsics; RecompXmm keeps its layout.

    The union is untouched (no __m128 member, so no 16-byte alignment and no
    by-value aggregate the 32-bit MSVC conformance build would reject); the
    helpers load lanes with _mm_loadu_ps and store with _mm_storeu_ps, which
    the compiler turns into register operations once inlined. Scalar MEM*
    stay volatile. An XMM load becomes one 16-byte access -- what movaps
    actually does -- instead of four volatile dword loads."""
    h = _must_replace(h, "#ifndef RECOMP_XMM_DEFINED",
                      "#include <xmmintrin.h>\n#ifndef RECOMP_XMM_DEFINED",
                      "m128 include")
    h = _must_replace(
        h,
        "static inline RecompXmm XMM_MEM(uint32_t addr) {\n"
        "    RecompXmm r;\n"
        "    r.u[0] = MEM32(addr);      r.u[1] = MEM32(addr + 4);\n"
        "    r.u[2] = MEM32(addr + 8);  r.u[3] = MEM32(addr + 12);\n"
        "    return r;\n"
        "}\n",
        "static inline __m128 recomp_xmm_v(RecompXmm a) { return _mm_loadu_ps(a.f); }\n"
        "static inline RecompXmm recomp_xmm_r(__m128 v) {\n"
        "    RecompXmm r; _mm_storeu_ps(r.f, v); return r;\n"
        "}\n"
        "static inline RecompXmm XMM_MEM(uint32_t addr) {\n"
        "    return recomp_xmm_r(_mm_loadu_ps((const float *)XBOX_PTR(addr)));\n"
        "}\n",
        "m128 XMM_MEM")
    h = _must_replace(
        h,
        "static inline void XMM_STORE(uint32_t addr, RecompXmm v) {\n"
        "    MEM32(addr)      = v.u[0]; MEM32(addr + 4)  = v.u[1];\n"
        "    MEM32(addr + 8)  = v.u[2]; MEM32(addr + 12) = v.u[3];\n"
        "}\n",
        "static inline void XMM_STORE(uint32_t addr, RecompXmm v) {\n"
        "    _mm_storeu_ps((float *)XBOX_PTR(addr), recomp_xmm_v(v));\n"
        "}\n",
        "m128 XMM_STORE")
    for name, intr in (("XMM_ADD", "_mm_add_ps"), ("XMM_SUB", "_mm_sub_ps"),
                       ("XMM_MUL", "_mm_mul_ps"), ("XMM_DIV", "_mm_div_ps"),
                       ("XMM_MIN", "_mm_min_ps"), ("XMM_MAX", "_mm_max_ps")):
        m = re.search(r"RECOMP_XMM_LANEWISE\(%s, [^\n]*\)\n" % name, h)
        if not m:
            raise SystemExit(f"m128: {name} not found")
        h = (h[:m.start()]
             + f"static inline RecompXmm {name}(RecompXmm a, RecompXmm b) "
               f"{{ return recomp_xmm_r({intr}(recomp_xmm_v(a), recomp_xmm_v(b))); }}\n"
             + h[m.end():])
    m = re.search(r"static inline RecompXmm XMM_SHUFFLE\(RecompXmm a, RecompXmm b, "
                  r"uint32_t imm\) \{.*?\n\}\n", h, re.S)
    if not m:
        raise SystemExit("m128: XMM_SHUFFLE not found")
    # The lifter always passes the immediate as a literal.
    h = (h[:m.start()]
         + "#define XMM_SHUFFLE(a, b, imm) recomp_xmm_r(_mm_shuffle_ps("
           "recomp_xmm_v(a), recomp_xmm_v(b), (imm)))\n"
         + h[m.end():])
    return h


HEADERS = {
    "base": lambda h: h,
    "novol": header_novol,
    "notls": header_notls,
    "tlsstruct": header_tlsstruct,
    "m128": header_m128,
}


# ── source variants: models of emitter changes ───────────────────────────

def _func_split(c):
    m = re.search(r"void (sub_[0-9A-F]+)\(void\)\n\{\n", c)
    return c[:m.end()], c[m.end():]


def cache_registers(c, sync_at_calls=True):
    """Option (c)/(a') of the review, naively: load every GPR, every XMM the
    function names and the memory base into locals at entry; store them all
    back before every call and at every return; reload them all after a call."""
    head, body = _func_split(c)
    xmms = sorted(set(re.findall(r"\bxmm[0-7]\b", body)))
    regs = GPRS + xmms
    pre = [f"#undef {r}" for r in regs]
    pre += ["#undef XBOX_PTR",
            "#define XBOX_PTR(addr) ((uintptr_t)(uint32_t)(addr) + _base)"]
    decl = ["    const ptrdiff_t _base = g_xbox_mem_offset;",
            "    uint32_t " + ", ".join(f"{r} = g_{r}" for r in GPRS) + ";"]
    if xmms:
        decl.append("    RecompXmm " + ", ".join(f"{x} = g_{x}" for x in xmms) + ";")
    store = " ".join(f"g_{r} = {r};" for r in regs)
    load = " ".join(f"{r} = g_{r};" for r in regs)
    if "#define fp_push(v)" in body:
        # The x87 stack too: the array and its top into locals. The index is
        # still a run-time value, so the array stays in (host stack) memory.
        macros, rest = body.split("\nloc_", 1)
        macros = (macros.replace("g_fp_top", "_fpt").replace("g_fp_stack[", "_fps["))
        body = macros + "\nloc_" + rest
        decl.append("    double _fps[8]; unsigned _fpt = (unsigned)g_fp_top;"
                    " memcpy(_fps, g_fp_stack, sizeof _fps);")
        fp_store = " memcpy(g_fp_stack, _fps, sizeof _fps); g_fp_top = (int)_fpt;"
        fp_load = " memcpy(_fps, g_fp_stack, sizeof _fps); _fpt = (unsigned)g_fp_top;"
        store += fp_store
        load += fp_load
    body = re.sub(r"\breturn;", "{ " + store + " return; }", body)
    if sync_at_calls:
        body = re.sub(r"(RECOMP_ABI_CALL(?:_POP)?\([^;]*\);)",
                      "{ " + store + r" } \1 { " + load + " }", body)
    end = body.rindex("}")
    post = [f"#define {r} g_{r}" for r in regs]
    post += ["#undef XBOX_PTR",
             "#define XBOX_PTR(addr) ((uintptr_t)(uint32_t)(addr) + g_xbox_mem_offset)"]
    return (head + "\n".join(pre) + "\n" + "\n".join(decl) + "\n"
            + body[:end + 1] + "\n" + "\n".join(post) + "\n" + body[end + 1:])


_STOSD_LOOP = ("{ uint32_t _i; int32_t _st = RECOMP_DF_STEP(4); for (_i = 0; "
               "_i < ecx; _i++) MEM32(edi + _i*_st) = eax; edi += ecx * _st; }")
_STOSD_FAST = ("{ uint32_t _n = ecx, _v = eax, _d = edi; "
               "if (!g_df && (_v & 0xFFu) * 0x01010101u == _v) "
               "memset((void*)XBOX_PTR(_d), (int)(_v & 0xFFu), (size_t)_n * 4u); "
               "else { uint32_t _i; int32_t _st = RECOMP_DF_STEP(4); "
               "for (_i = 0; _i < _n; _i++) MEM32(_d + _i*_st) = _v; } "
               "edi = _d + _n * (uint32_t)RECOMP_DF_STEP(4); }")


def stosd_fast_path(c):
    """rep stosd: snapshot ecx/eax/edi into locals (no reload per dword), and
    memset when DF is clear and eax is one byte repeated -- a clear, nearly
    always. Mirrors what rep stosb already does."""
    return _must_replace(c, _STOSD_LOOP, _STOSD_FAST, "stosd fast path", None)


_FULL_STORE = "{ " + " ".join(f"g_{r} = {r};" for r in GPRS)
_FULL_LOAD = "{ " + " ".join(f"{r} = g_{r};" for r in GPRS) + " }"


def calls_liveness_sync(caller, callee):
    """What a liveness-aware emitter would write for caller.s/callee.s: store
    only registers written since the last sync, reload only registers read
    before being written after the call. Hand-applied to this pair; the
    emitter would compute both sets from the CFG it already builds."""
    caller = cache_registers(caller)
    caller = _must_replace(caller, _FULL_STORE + " } RECOMP_ABI_CALL",
                           "{ g_esp = esp; g_esi = esi; g_edi = edi; } RECOMP_ABI_CALL",
                           "liveness: store before call")
    caller = _must_replace(caller, "sub_00090100); " + _FULL_LOAD,
                           "sub_00090100); { eax = g_eax; esp = g_esp; "
                           "esi = g_esi; edi = g_edi; }",
                           "liveness: reload after call")
    caller = _must_replace(caller, _FULL_STORE + " return; }",
                           "{ g_esi = esi; g_edi = edi; g_esp = esp; return; }",
                           "liveness: caller exit")
    callee = cache_registers(callee)
    callee = _must_replace(callee, _FULL_STORE + " return; }",
                           "{ g_eax = eax; g_ecx = ecx; g_esp = esp; return; }",
                           "liveness: callee exit")
    return caller, callee


def x87_static_slots(c):
    """x87 with the stack depth tracked by the lifter rather than at run time:
    every slot this function pushes is a named double. The depth is static in
    this loop (fldz leaves +1; each iteration pushes one and pops one), which
    is the usual case for compiler-generated x87."""
    reps = [
        ("    fp_push(0.0); /* fldz */", "    double _s1 = 0.0; /* fldz: depth +1 */"),
        ("    fp_push(MEMF(eax)); /* fld float */\n"
         "    fp_top() = fp_top() * MEMF(eax + 4); /* fmul dword ptr [eax + 4] */\n"
         "    fp_st1() = fp_st1() + fp_top(); fp_pop(); /* faddp st(1) */",
         "    { double _s2 = MEMF(eax); _s2 = _s2 * MEMF(eax + 4);"
         " _s1 = _s1 + _s2; } /* fld; fmul; faddp: depth +2, +1 */"),
        ("    fp_top() = sqrt(fp_top()); /* fsqrt */", "    _s1 = sqrt(_s1); /* fsqrt */"),
        ("    MEMF(eax) = (float)fp_top(); fp_pop(); /* fstp */",
         "    MEMF(eax) = (float)_s1; /* fstp: depth back to 0 */"),
    ]
    for old, new in reps:
        c = _must_replace(c, old, new, "x87 static slots")
    return c


# variant -> (header, {bench: transform(list of lifted C) -> list of C})
VARIANTS = {
    "base":        ("base", {}),
    "novol":       ("novol", {}),
    "notls":       ("notls", {}),
    "tlsstruct":   ("tlsstruct", {}),
    "stosd":       ("base", {"clear": lambda cs: [stosd_fast_path(cs[0])]}),
    "m128":        ("m128", {}),
    "locals":      ("base", {b: (lambda cs: [cache_registers(x) for x in cs])
                             for b in BENCHES}),
    "locals_m128": ("m128", {"matmul": lambda cs: [cache_registers(cs[0])]}),
    "liveness":    ("base", {"calls": lambda cs: list(calls_liveness_sync(*cs)),
                             "x87sum": lambda cs: [x87_static_slots(cs[0])]}),
    # The real thing rather than a model of it: lifted by tools.recomp with
    # --perf-opts (third field), against the unmodified header.
    "perf_cheap":  ("base", {}, frozenset({"stosd", "rmw-snapshot", "fcmp-float",
                                           "xmm-intrinsics"})),
    "perf_all":    ("base", {}, frozenset(perf_opts.OPTS)),
}


def variant_opts(v):
    return VARIANTS[v][2] if len(VARIANTS[v]) > 2 else frozenset()


# ── build and run ────────────────────────────────────────────────────────

def count_instructions(obj, func):
    out = subprocess.run(["objdump", "-d", "--no-show-raw-insn", obj],
                         capture_output=True, text=True, check=True).stdout
    n, inside = 0, False
    for line in out.splitlines():
        if line.endswith(f"<{func}>:"):
            inside = True
            continue
        if inside:
            if not line.strip():
                break
            n += 1
    return n


def main(argv=None):
    ap = argparse.ArgumentParser(prog="python3 -m tools.codegen_bench",
                                 description=__doc__.split("\n\n")[0])
    ap.add_argument("--cc", default="gcc,clang")
    ap.add_argument("--variants", default=",".join(VARIANTS))
    ap.add_argument("--opt", default="-O2")
    ap.add_argument("--reps", type=int, default=5)
    ap.add_argument("--workdir", default=os.path.join(HERE, "build"))
    ap.add_argument("--show", metavar="BENCH:VARIANT",
                    help="print the C a variant compiles for one bench and exit")
    args = ap.parse_args(argv)

    os.makedirs(args.workdir, exist_ok=True)
    lifted_by = {}

    def lifted_for(opts):
        if opts not in lifted_by:
            lifted_by[opts] = {
                b: [lift(os.path.join(HERE, "snippets", f), va, args.workdir, opts)
                    for f, va in snips]
                for b, (snips, _, _) in BENCHES.items()}
        return lifted_by[opts]
    with open(RUNTIME_HEADER) as f:
        header = f.read()

    def sources(variant, bench):
        lifted = lifted_for(variant_opts(variant))
        transform = VARIANTS[variant][1].get(bench)
        return transform(lifted[bench]) if transform else lifted[bench]

    if args.show:
        bench, variant = args.show.split(":")
        print("\n\n".join(sources(variant, bench)))
        return 0

    compilers = [c for c in args.cc.split(",") if shutil.which(c)]
    if not compilers:
        raise SystemExit("no compiler found (wanted %s)" % args.cc)
    variants = args.variants.split(",")
    results, checks = [], {}
    for v in variants:
        hdir = os.path.join(args.workdir, "hdr_" + VARIANTS[v][0])
        os.makedirs(hdir, exist_ok=True)
        with open(os.path.join(hdir, "recomp_types.h"), "w") as f:
            f.write(HEADERS[VARIANTS[v][0]](header))
        srcs = []
        for b in BENCHES:
            path = os.path.join(args.workdir, f"{b}.{v}.c")
            with open(path, "w") as f:
                f.write('#define RECOMP_GENERATED_CODE\n'
                        + "".join(d + "\n" for d in perf_defines(variant_opts(v)))
                        + '#include "recomp_types.h"\n'
                        "#include <string.h>\nvoid sub_00090100(void);\n\n"
                        + "\n".join(sources(v, b)))
            srcs.append((b, path))
        for cc in compilers:
            flags = [args.opt, "-I", hdir, "-fno-asynchronous-unwind-tables"]
            objs, counts = [], {}
            for b, src in srcs:
                obj = src[:-2] + f".{cc}.o"
                subprocess.check_call([cc] + flags + ["-c", src, "-o", obj])
                objs.append(obj)
                func = "sub_%08X" % BENCHES[b][0][0][1]
                counts[b] = count_instructions(obj, func)
            exe = os.path.join(args.workdir, f"bench_{v}_{cc}")
            subprocess.check_call([cc] + flags + ["-o", exe,
                                  os.path.join(HERE, "bench.c")] + objs + ["-lm"])
            times = {}
            for b, (_, case, iters) in BENCHES.items():
                best = None
                for _ in range(args.reps):
                    out = subprocess.run([exe, str(case), str(iters)],
                                         capture_output=True, text=True)
                    if out.returncode:
                        raise SystemExit(f"{v}/{cc}/{b} failed: {out.stderr}")
                    ns, check = out.stdout.split()
                    best = float(ns) if best is None else min(best, float(ns))
                    checks.setdefault(b, {})[f"{v}/{cc}"] = check
                times[b] = best
            results.append((v, cc, times, counts))

    print(f"ns per call, best of {args.reps} ({args.opt}); "
          "host instructions in the lifted function in brackets")
    print(f"{'variant':<12} {'cc':<6}" + "".join(f"{b:>18}" for b in BENCHES))
    for v, cc, times, counts in results:
        print(f"{v:<12} {cc:<6}" + "".join(
            f"{times[b]:>11.1f} [{counts[b]:>4}]" for b in BENCHES))
    bad = [b for b, seen in checks.items() if len(set(seen.values())) != 1]
    if bad:
        for b in bad:
            print(f"\nMISMATCH in {b}: {checks[b]}")
        return 1
    print("\nAll builds wrote identical guest memory.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
