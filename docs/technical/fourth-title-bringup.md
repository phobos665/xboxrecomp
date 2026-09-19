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

Two readings, and they need different work:

1. **An aperture we do not map.** If the title computes a write-combined
   address with a base or a bit we do not model, the fix is in
   `xbox_memory_layout.c` and would likely help other titles.
2. **A pointer that was never valid.** An uninitialised object, or a vtable
   or indirect call that went somewhere wrong, would produce a plausible-
   looking high address just as easily. 73 call targets are stubbed as
   unresolved in this title.

Nothing distinguishes them yet. The cheap test is to find where 0xF8604020 is
*written*: walk back from `sub_001BD668` and `sub_001BDCE5` in the call chain
to whatever constructs that object, and see whether the address is computed
from a base (reading 1) or read out of memory (reading 2).

## Next

1. Work out which of the two readings above is right, as above.
2. Then the ordinary boot loop: `docs/technical/second-title-bringup.md` is the
   worked example of what that looks like, and `CLAUDE.md`'s debug table maps
   symptoms to causes.
3. The demand-loaded sections (four of them) have not been exercised yet and
   are worth remembering when something later is mysteriously absent.
