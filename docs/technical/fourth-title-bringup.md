# Fourth title: Outrun 2, XDK 5849

Started 19 September 2026, after Marvel vs Capcom 2 was parked. A racing game,
so the nearest thing yet to Burnout 2 — and on **XDK 5849**, the version the
toolkit was originally built against, which should mean the fewest surprises
of any title so far.

## What it is

| | |
| --- | --- |
| Title ID | 0x53450036 |
| XDK build | **5849** (Burnout 2 is 5344, TimeSplitters 2 is 4721, Marvel vs Capcom 2 is 5028) |
| Graphics | D3D8, not LTCG |
| Engine | not RenderWare |
| `.text` | 1,540 KB — the smallest of the four |
| Sections | 22, **four demand-loaded** |
| Kernel imports | 152 |
| Entry point | 0x000F33CD |
| Notable libraries | XONLINES, XVOICE, VOICMAIL — it has Xbox Live and voice |

## Getting to a build

Again nothing in the pipeline needed changing:

```
py -3 -m tools.xdk_symbols "games/Outrun 2/default.xbe"
py -3 scripts/recompile.py "games/Outrun 2/default.xbe" \
    --work-dir games/_pipeline/outrun2/out --project titles/outrun2
py -3 scripts/regen_title_main.py outrun2 "Outrun 2" 0x000F33CD "Outrun 2"
cmake -S titles/outrun2 -B titles/outrun2/build -G "Visual Studio 16 2019" -A x64
cmake --build titles/outrun2/build --config Release --target outrun2_recomp
```

11,812 functions, 1,331,762 lines of C in 12 files, none failed, 73 call
targets stubbed as unresolved. **48 of 53 XDK functions replaced by name** —
the best of the four, which is what being on 5849 buys.

## Where it stops

Further than Marvel vs Capcom 2 in one way and less far in another: it reaches
`CreateDevice`, creates the shadow device at 640x480, mirrors the GPU time
fence, finds its frame buffer at physical 0x024E4000, presents its first
`Swap` — and then **faults**.

```
[CRASH] Access violation at RIP=0x7FF7CAF11DBF, fault addr=0xF861404C (read)
  Xbox regs: eax=0x0000000D ecx=0xF8604020 esi=0xF8604020 esp=0x00F7EF04
  Xbox VA of fault: 0xF860404C
  in sub_001BB846+0xCF
  guest stack: 0x00257360 <- 0x001BC6D2 <- 0x001BD546 <- 0x001BD668 <- 0x001BDCE5
```

`sub_001BB846` is a thiscall method — its first act is `esi = ecx` — so
**0xF8604020 is the object pointer itself**, and the fault is reading a field
of it. The address is in no aperture this runtime maps: the tiled aperture
covers 64 MB at 0xF0000000, and 0xF8604020 is 140 MB past its base, which is
more physical memory than the console has.

Two readings were possible, and they needed different work: an aperture this
runtime does not map, or a pointer that was never valid. **It is the second,
and the evidence is conclusive.**

`RECOMP_ALIAS_HIGH=1` maps 0xF8000000 as a second alias of the contiguous
window, purely to answer this. With it, the title reads that address happily
and then faults immediately at **0xFFC00000** — a different wild address, in
different code. Following a bad pointer looks exactly like that; a real
aperture does not.

What clinches it is what those numbers are. Searching the XBE:

| value | occurrences in the image | what it is |
| --- | --- | --- |
| 0xF8604020 | 0 | computed at run time, not a constant |
| 0xFFC00000 | **1548** | the bit pattern of a NaN float |

0xFFC00000 is `-NaN` in IEEE 754, and 1548 of them are sitting in the title's
data — uninitialised or "invalid" float slots. The second fault dereferences
one. So the code is reading **float data as a pointer**, which means the
structure it is walking was never filled in.

`RECOMP_ALIAS_HIGH` is kept, off by default and labelled an experiment,
because it is the probe that answered the question and the question will come
up again. It should never be turned on to make a title "work": all it does is
let a bad pointer read zeros instead of faulting, which trades a clear failure
for a mysterious one.

## What the object is

The crash handler now prints the raw stack as well as the return addresses,
which is what made the rest of this findable — the useful word was a data
pointer the filtered list threw away.

The saved registers put **0x00257360** in the frame. That address is in
`.rdata`, is not writable, and holds eight pointers into `.text`: it is a
**vtable**. So this is a C++ object, and the field being dereferenced is one
of its members.

Two constructors install that vtable, and comparing them says what the member
is:

```
sub_001BB69D:  MEM32(eax)     = 0x257360;      /* vtable */
               ecx            = MEM32(ecx + 0x18);
               MEM32(eax + 4) = ecx;           /* +4 <- a dword from elsewhere */

sub_001BB27E:  MEM8(eax + 4)  = ...;           /* +4 <- four separate bytes */
               MEM8(eax + 5)  = ...;
               MEM8(eax + 6)  = ...;
               MEM32(eax)     = 0x257360;      /* vtable */
               MEM8(eax + 7)  = ...;
```

One writes four bytes into +4..+7; the other writes a dword. So **+4 is a
four-byte payload whose meaning depends on how the object was made** — a
variant, or a property holding either inline data or a reference.

The caller that crashes, `sub_001BC6B2`, does `ecx = MEM32(ecx + 4)` and then
calls a method on the result: it is treating the payload as an object
pointer. The payload holds float data. So either the object was built by the
wrong constructor, or the value handed to `sub_001BB69D` from `[ecx + 0x18]`
was already wrong.

That is a much smaller question than "why does it crash", and it is where the
next session should start: instrument `sub_001BB69D` to record what it is
given, and find what writes `+0x18` of the object that feeds it.

## Next

1. **Find what feeds the payload**, as above: `sub_001BB69D`'s source
   `[ecx + 0x18]`, and which constructor actually built the instance that
   crashes. Static analysis plus one instrumented run.
2. Then the ordinary boot loop: `docs/technical/second-title-bringup.md` is the
   worked example of what that looks like, and `CLAUDE.md`'s debug table maps
   symptoms to causes.
3. The demand-loaded sections (four of them) have not been exercised yet and
   are worth remembering when something later is mysteriously absent.
