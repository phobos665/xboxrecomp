"""Guest functions for the A/B runner: x86-32 bytes and the VA they live at.

Three sources, none of them game code:

  conformance  every tools/conformance case (the lifter's semantic corpus),
               with a `ret` appended so each is a leaf function
  bench        tools/codegen_bench/snippets, which are shaped like the hot
               functions the lifted-code review measured
  generated    seeded random leaf functions: straight-line ALU, memory
               read-modify-writes at every width, string ops, SSE and x87
               blocks, forward branches and short counted loops -- the
               shapes the perf options rewrite

Assembled with GNU as (Intel syntax), so this runs anywhere binutils does.
"""

import os
import random
import re
import subprocess
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
BENCH_SNIPPETS = os.path.join(ROOT, "tools", "codegen_bench", "snippets")

# Snippets are laid out from here, one 4 KB slot each, so call targets and
# absolute references never collide with the runner's data region.
CODE_BASE = 0x00020000


def _masm_to_gas(line):
    """MSVC inline-asm spellings GNU as does not take: 07FFFFFFFh hex."""
    return re.sub(r"\b0*([0-9A-Fa-f]+)[hH]\b",
                  lambda m: "0x" + m.group(1), line)


def assemble(lines_or_text, va, workdir):
    """Assemble at `va` (so rel32 calls resolve) and return the bytes."""
    text = lines_or_text if isinstance(lines_or_text, str) else "\n".join(lines_or_text)
    if ".intel_syntax" not in text:
        text = ".intel_syntax noprefix\n.code32\n.text\n" + text + "\n"
    fd, path = tempfile.mkstemp(suffix=".s", dir=workdir)
    with os.fdopen(fd, "w") as f:
        f.write(text)
    obj, elf, binp = path[:-2] + ".o", path[:-2] + ".elf", path[:-2] + ".bin"
    try:
        subprocess.run(["as", "--32", "-o", obj, path], check=True,
                       capture_output=True, text=True)
        subprocess.run(["ld", "-m", "elf_i386", f"-Ttext=0x{va:x}", "-e",
                        f"0x{va:x}", "-o", elf, obj], check=True,
                       capture_output=True, text=True)
        subprocess.run(["objcopy", "-O", "binary", "-j", ".text", elf, binp],
                       check=True, capture_output=True, text=True)
        with open(binp, "rb") as f:
            return f.read()
    except subprocess.CalledProcessError as exc:
        raise ValueError(exc.stderr.strip().splitlines()[-1] if exc.stderr else str(exc))
    finally:
        for p in (path, obj, elf, binp):
            if os.path.exists(p):
                os.remove(p)


def conformance_sources():
    from ..conformance.cases import CASES
    for c in CASES:
        yield "conf_" + c["name"], [_masm_to_gas(l) for l in c["asm"]] + ["ret"]


def bench_sources():
    for name in sorted(os.listdir(BENCH_SNIPPETS)):
        if name.endswith(".s"):
            with open(os.path.join(BENCH_SNIPPETS, name)) as f:
                yield "bench_" + name[:-2], f.read()


# ── generated leaves ─────────────────────────────────────────────────────

# The runner seeds esi, edi and ebx as pointers into mapped memory, so the
# generator only ever addresses through them and never computes into them:
# arithmetic on a pointer register would send the next access off the map,
# and a run that faults in the reference build says nothing.
_PTR = ["esi", "edi", "ebx"]
_R32 = ["eax", "ecx", "edx"]
_R16 = ["ax", "cx", "dx"]
_R8 = ["al", "cl", "dl", "ah", "ch", "dh"]
_SIZE = {1: "byte", 2: "word", 4: "dword"}


def _mem(rng, size):
    base = rng.choice(_PTR)
    disp = rng.choice([0, 4, 8, 0x10, 0x3C, -4, 0x100])
    d = f" + {disp}" if disp > 0 else (f" - {-disp}" if disp < 0 else "")
    return f"{_SIZE[size]} ptr [{base}{d}]"


def _reg(rng, size):
    return rng.choice({4: _R32, 2: _R16, 1: _R8}[size])


def _alu(rng):
    """One flag-setting instruction, then sometimes a consumer of its flags."""
    size = rng.choice([4, 4, 4, 2, 1])
    op = rng.choice(["add", "sub", "and", "or", "xor", "adc", "sbb",
                     "inc", "dec", "neg", "shl", "shr", "sar", "cmp", "test"])
    dst = _mem(rng, size) if rng.random() < 0.6 else _reg(rng, size)
    if op in ("inc", "dec", "neg"):
        out = [f"{op} {dst}"]
    elif op in ("shl", "shr", "sar"):
        out = [f"{op} {dst}, {rng.choice(['1', '3', '7', 'cl'])}"]
    else:
        src = rng.choice([_reg(rng, size), str(rng.choice([1, 0x7F, 0x80, 0xFF, 5]))])
        if op in ("adc", "sbb") and rng.random() < 0.5:
            out = ["stc" if rng.random() < 0.5 else "clc"]
        else:
            out = []
        out.append(f"{op} {dst}, {src}")
    use = rng.random()
    if use < 0.3:
        out.append(f"set{rng.choice(['e', 'ne', 'l', 'g', 'b', 'a', 's', 'be', 'ge'])} "
                   f"{rng.choice(_R8)}")
    elif use < 0.45:
        out.append(f"cmov{rng.choice(['e', 'ne', 'l', 'b', 's'])} "
                   f"{rng.choice(_R32)}, {rng.choice(_R32)}")
    elif use < 0.55:
        out.append(f"adc {rng.choice(_R32)}, 0")
    return out


def _string_op(rng):
    op = rng.choice(["rep stosd", "rep stosw", "rep stosb", "rep movsd",
                     "rep movsb", "stosd", "lodsd"])
    out = [f"mov ecx, {rng.choice([0, 1, 3, 16, 65])}"]
    if rng.random() < 0.5:
        out.append("xor eax, eax")
    elif rng.random() < 0.5:
        out.append("mov eax, 0x5A5A5A5A")
    if rng.random() < 0.15:
        out = ["std"] + out + [op, "cld"]
        return out
    return out + [op]


def _sse(rng):
    x = lambda: f"xmm{rng.randrange(8)}"
    op = rng.choice(["addps", "subps", "mulps", "minps", "maxps", "andps",
                     "orps", "xorps", "andnps", "unpcklps", "unpckhps",
                     "cmpltps", "cmpneqps"])
    src = f"xmmword ptr [{rng.choice(_PTR)}]" if rng.random() < 0.3 else x()
    out = [f"{op} {x()}, {src}"]
    r = rng.random()
    if r < 0.25:
        out.append(f"shufps {x()}, {x()}, {rng.randrange(256)}")
    elif r < 0.45:
        out.append(f"movups xmmword ptr [{rng.choice(_PTR)} + 0x20], {x()}")
    elif r < 0.6:
        out.append(f"movmskps {rng.choice(_R32)}, {x()}")
    elif r < 0.8:
        out += [f"comiss {x()}, {x()}",
                f"set{rng.choice(['a', 'b', 'e', 'be', 'ae', 'p', 'np'])} "
                f"{rng.choice(_R8)}"]
    return out


def _x87(rng):
    m = f"dword ptr [{rng.choice(_PTR)} + {rng.choice([0, 4, 8])}]"
    return rng.choice([
        ["fld " + m, "fadd " + m, "fstp " + m],
        ["fld " + m, "fld " + m, "fmulp st(1), st", "fistp " + m],
        ["fld " + m, "fabs", "fsqrt", "fstp dword ptr [esi + 0x40]"],
    ])


def _block(rng):
    k = rng.random()
    if k < 0.55:
        return _alu(rng)
    if k < 0.7:
        return _string_op(rng)
    if k < 0.85:
        return _sse(rng)
    if k < 0.93:
        return _x87(rng)
    return [f"mov {_mem(rng, 4)}, {rng.choice(_R32)}",
            f"mov {rng.choice(_R32)}, {_mem(rng, 4)}"]


def generated_sources(count, seed=1):
    rng = random.Random(seed)
    for n in range(count):
        lines, label = [], 0
        for _ in range(rng.randint(3, 10)):
            if rng.random() < 0.2:
                # A forward branch over the next block.
                label += 1
                lines += _alu(rng)[:1] + [
                    f"j{rng.choice(['e', 'ne', 'l', 'ge', 'b', 'ae', 's'])} .Lf{label}"]
                lines += _block(rng) + [f".Lf{label}:"]
            elif rng.random() < 0.08:
                # A short counted loop: the back edge the register cache
                # must carry its locals around.
                label += 1
                lines += [f"mov ecx, {rng.randint(1, 5)}", f".Ll{label}:"]
                lines += [l for l in _alu(rng)
                          if not re.search(r"\b(ecx|cx|cl|ch)\b", l)]
                lines += [f"dec ecx", f"jnz .Ll{label}"]
            else:
                lines += _block(rng)
        lines.append("ret" if rng.random() < 0.8 else f"ret {rng.choice([4, 8])}")
        yield f"gen_{n:04d}", lines


def build(workdir, generated=200, seed=1, sources=("conformance", "bench", "generated")):
    """[(name, va, bytes)], skipping anything the assembler refuses."""
    out, skipped = [], []
    va = CODE_BASE
    gens = []
    if "conformance" in sources:
        gens.append(conformance_sources())
    if "bench" in sources:
        gens.append(bench_sources())
    if "generated" in sources and generated:
        gens.append(generated_sources(generated, seed))
    for gen in gens:
        for name, src in gen:
            # The bench snippets are linked at their own VAs (callee/caller
            # call each other by absolute target), so keep those.
            fixed = {"bench_caller": 0x00090200, "bench_callee": 0x00090100}
            at = fixed.get(name, va)
            try:
                code = assemble(src, at, workdir)
            except ValueError as exc:
                skipped.append((name, str(exc)))
                continue
            out.append((name, at, code))
            if name not in fixed:
                va += 0x1000
    return out, skipped
