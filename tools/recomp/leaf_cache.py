"""--perf-opts leaf-cache: guest registers in C locals, for functions that call
nothing.

The guest registers are thread-local globals (recomp_types.h), and a guest
store goes through a pointer the compiler cannot tell apart from them, so it
reloads every register after every store. A C local cannot be aliased. So a
function that never hands control to anything else loads the registers it
names into locals at entry and stores them back at every exit -- and since
nothing outside the function can run in between, nothing can see the
difference.

"Calls nothing" is decided on the C, conservatively. The function qualifies
only if every function-like name in its body is one of:

  - a helper or macro from recomp_types.h, other than the ones that dispatch
    to other code or touch the register globals themselves (the ICALL and
    ITAIL family, RECOMP_ABI_CALL, the HLE and trace macros);
  - a C library math or memory function;
  - the per-function x87 stack macros;

and it names no register global (g_eax ...) directly. A call to a lifted
function, a tail jump, a fall-through into the next function, an int3 report
or an unimplemented-instruction hook all fail that test, and such a function
is emitted exactly as before. So does anything this module does not
recognise.

What changes for diagnostics: a thread inside a cached function shows the
registers as they were at its entry to anything that reads the globals from
outside -- the watchdog's register line, a crash dump, a watchpoint report.
Lift without leaf-cache when hunting a bug with those.

The memory base is cached too, as a const local: MSVC does no type-based
alias analysis and otherwise reloads g_xbox_mem_offset after every guest
store as well.
"""

import os
import re

GPRS = ("eax", "ecx", "edx", "ebx", "esi", "edi", "esp")

_HEADER = os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__)))), "templates", "runtime", "recomp_types.h")

# Header names that run other code or read/write the register globals.
_DENY_PREFIXES = ("RECOMP_ICALL", "RECOMP_ITAIL", "RECOMP_ABI_CALL",
                  "RECOMP_HLE", "RECOMP_TRACE")

_C_NAMES = frozenset({
    # keywords and operators that take parentheses
    "if", "for", "while", "switch", "return", "sizeof", "do",
    # C library: pure functions of their arguments, or guest-memory copies
    "sqrt", "sqrtf", "floor", "floorf", "ceil", "ceilf", "trunc", "truncf",
    "fabs", "fabsf", "fmod", "fmodf", "ldexp", "frexp", "exp", "exp2", "log",
    "log2", "log10", "pow", "sin", "cos", "tan", "atan", "atan2", "rint",
    "nearbyint", "round", "copysign", "isnan", "isinf", "isfinite", "signbit",
    "memset", "memcpy", "memmove",
    # the x87 stack macros translate_function defines per function
    "fp_push", "fp_pop", "fp_top", "fp_st", "fp_st1",
    # reads the time-stamp counter; touches no guest register
    "xbox_ReadTimeStampCounter",
})

_allowed = None


def allowed_names():
    """Every function-like name a cached function may use."""
    global _allowed
    if _allowed is None:
        names = set(_C_NAMES)
        try:
            with open(_HEADER, encoding="utf-8") as f:
                h = f.read()
            names |= set(re.findall(r"#\s*define\s+(\w+)\(", h))
            names |= set(re.findall(r"static inline [\w\s\*]+?\b(\w+)\s*\(", h))
            # Helpers stamped out by a generator macro at file scope:
            # RECOMP_XMM_LANEWISE(XMM_ADD, ...), RECOMP_MMX_BINOP(...), ...
            names |= set(re.findall(r"^RECOMP_[A-Z0-9_]+\(\s*(\w+)\s*,", h, re.M))
        except OSError:
            pass       # no header beside us: nothing qualifies but the C list
        _allowed = frozenset(n for n in names if not n.startswith(_DENY_PREFIXES))
    return _allowed


_COMMENT = re.compile(r"/\*.*?\*/|//[^\n]*", re.S)
_CALLISH = re.compile(r"\b([A-Za-z_]\w*)\s*\(")
_REG_GLOBAL = re.compile(r"\bg_(eax|ecx|edx|ebx|esi|edi|esp)\b")


def _code_only(text):
    return _COMMENT.sub(" ", text)


def why_not(c_text):
    """None if the function can keep its registers in locals, else why not."""
    head, _, body = c_text.partition("\n{\n")
    if not body:
        return "no function body found"
    code = _code_only(body)
    if _REG_GLOBAL.search(code):
        return "names a register global directly"
    allowed = allowed_names()
    for m in _CALLISH.finditer(code):
        name = m.group(1)
        # A cast such as (uint32_t)(x) puts a type name before "(".
        if name.endswith("_t") or name in ("int", "unsigned", "float", "double",
                                           "char", "short", "long", "void"):
            continue
        if name not in allowed:
            return f"calls {name}"
    return None


def apply(c_text):
    """The same function with its registers cached, or c_text unchanged."""
    if why_not(c_text) is not None:
        return c_text
    sig_end = c_text.index("\n{\n") + 3
    head, body = c_text[:sig_end], c_text[sig_end:]
    code = _code_only(body)
    regs = [r for r in GPRS if re.search(r"\b%s\b" % r, code)]
    xmms = sorted(set(re.findall(r"\bxmm[0-7]\b", code)))
    names = regs + xmms
    if not names:
        return c_text

    pre = [f"#undef {n}" for n in names]
    pre += ["#undef XBOX_PTR",
            "#define XBOX_PTR(addr) ((uintptr_t)(uint32_t)(addr) + _base)",
            "    /* leaf-cache: calls nothing, so the registers live in locals"
            " and go back at every exit */",
            "    const ptrdiff_t _base = g_xbox_mem_offset;"]
    if regs:
        pre.append("    uint32_t " + ", ".join(f"{r} = g_{r}" for r in regs) + ";")
    if xmms:
        pre.append("    RecompXmm " + ", ".join(f"{x} = g_{x}" for x in xmms) + ";")
    store = " ".join(f"g_{n} = {n};" for n in names)

    # Every return gets the store; comments are left alone.
    out, pos = [], 0
    for m in _COMMENT.finditer(body):
        out.append(re.sub(r"\breturn;", "{ " + store + " return; }",
                          body[pos:m.start()]))
        out.append(m.group(0))
        pos = m.end()
    out.append(re.sub(r"\breturn;", "{ " + store + " return; }", body[pos:]))
    body = "".join(out)

    # And falling off the end, which a function whose last instruction is not
    # a ret does (the lifter then emits no return at all).
    close = body.rstrip().rfind("\n}")
    if close < 0:
        return c_text
    body = (body[:close] + "\n    " + store + body[close:].rstrip() + "\n"
            + "\n".join(f"#define {n} g_{n}" for n in names) + "\n"
            + "#undef XBOX_PTR\n"
            "#define XBOX_PTR(addr) ((uintptr_t)(uint32_t)(addr) + g_xbox_mem_offset)\n")
    return head + "\n".join(pre) + "\n" + body
