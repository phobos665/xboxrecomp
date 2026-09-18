# A second title: TimeSplitters 2 (XDK 4721) beside Burnout 2 (XDK 5344)

The strategy in `CLAUDE.md` says a second title turns "it only does one game"
into a work queue: anything that breaks is something the toolkit wrongly
treated as universal. This is that queue, from bringing up TimeSplitters 2
(title 4553000A, XDK 4721, D3D8 non-LTCG, 1.8 MB `.text`) on a fresh Windows
machine in September 2026, with Burnout 2 rebuilt beside it from the template
as the regression check.

Both titles live as projects under `titles/`, with their pipeline output under
`games/_pipeline/<name>/out`; see the section at the end.

## What broke, in the order the title hit it

Each item was a general fault, not a TimeSplitters quirk, and each fix is in
the toolkit rather than in the title's `recomp_manual.c`, which is still empty.

### 1. The crash handler hid the first fault (template)

The template's exception handler reported only access violations and returned
silently for any fault in the GPU register range. Burnout 2's first run on
this machine ended with exit code 0xC0000005 and nothing in the log. The fault
was a write to the vblank interrupt-enable register, on the PCRTC page the
runtime write-traps under `RECOMP_VBLANK`, whose handler
(`nv2a_intr_handle_write`) the template never called.

Fix: the template's handler routes faults in the trapped pages to the runtime's
decoders (NV2A interrupt page, APU registers, AC'97 page), reports every other
exception code, and prints the faulting instruction's bytes, which is what a
decoder needs when it refuses a form.

### 2. The retail disc check (kernel)

TimeSplitters 2's certificate allows only the DVD-X2 media type
(`AllowedMedia 0x2`; Burnout 2's is `0x400001FF`). XAPI therefore verifies
the disc before `main`: it opens `\Device\CdRom0` itself, with nothing after
it, and sends MODE SENSE(10) for page 0x3E through
`IOCTL_SCSI_PASS_THROUGH_DIRECT`, up to five times. The bare device path had
no translation rule, `NtOpenFile` returned `STATUS_OBJECT_PATH_NOT_FOUND`, and
the title called `XLaunchNewImage(NULL)` -- back to the dashboard after
fifteen kernel calls, with nothing in its own log to say why.

Fix: the bare device maps to the game directory and opens as a directory
handle; the bridge answers the mode sense with the page a console presents
once its kernel has authenticated the disc (partition 1, CDF valid,
authenticated, book type 0xD; layout from xboxdevwiki's DVD Drive page, the
same three flags Cxbx-Reloaded sets).

### 3. The GPU time fence (HLE)

D3D8 stamps the push buffer with a rising fence value and keeps the value the
GPU has reached in a word of guest memory the device points at, written by
the GPU's interrupt. `D3D_BlockOnTime` spins until that word catches up, and
`KickOffAndWaitForIdle`, `BlockOnFence`, `BlockOnResource` and `Swap` all wait
through it. Nothing raises that interrupt here, so the title stopped after its
first `Swap`, 270 functions in, with its vblank ISR still ticking.

The kernel already had the mechanism (`xbox_Nv2aMirrorFence`: copy the
submitted value onto the completed word every poll), but nothing registered
it, and it needs two device-struct offsets that move between XDK builds
(5344: +0x2C submitted, +0x30 pointer to completed; 4721: +0x30 and +0x34).

Fix: the `Direct3D_CreateDevice` replacement reads the offsets from
`D3D_BlockOnTime`'s own prologue:

    56                push esi
    8B 35 <g_pDevice> mov  esi, [D3D_g_pDevice]
    8B 46 <A>         mov  eax, [esi + A]     ; pointer to the completed word
    8B 08             mov  ecx, [eax]
    8B 46 <B>         mov  eax, [esi + B]     ; last submitted value

Identical in both builds apart from A and B. For that, `HLE_IMPORT_VAR` now
resolves function names too: the address of a function is as good an import as
the address of a variable when what the replacement wants is to read its code.

### 4. The APU state was never stored (template)

`mcpx_apu_init_standalone` returns the device and does not assign
`g_apu_state`; the fault handler declines every access while it is NULL. The
template dropped the pointer, so with `RECOMP_AC97_READY` the first APU
register DirectSound touched (`NV_PAPU_FECTL`) was a crash.

### 5. The DSP doorbell is derivable after all (APU)

DirectSound hands the audio DSP a command word in guest RAM and spins until
the DSP clears it. The runtime's note said the address was not derivable from
the APU registers and had to be given per title (`RECOMP_APU_DSP_ACK`); its
automatic scan over contiguous blocks found nothing on Burnout 2.

It is derivable, one indirection deeper. The command block is the first page
of the DSP's scratch memory, and DirectSound tells the hardware where that is:
it builds a scatter-gather table of `{physical page, 0}` entries (the
`MmGetPhysicalAddress` loop in its allocator) and writes the table's address
to `GPSADDR` (`EPSADDR` for the encode processor). Entry 0 plus 0x810 is the
doorbell. TimeSplitters 2's `0x03650810` and Burnout 2's `0x00BB0810` and
`0x00BC0810` all come out of the registers now.

### 6. The APU read the wrong memory (template)

Reading that table exposed why the scan never worked: the APU addresses guest
memory physically, as `ram_ptr + (addr & 0x03FFFFFF)`, and the template handed
it the host address of guest VA 0. In this runtime physical page P is the
contiguous window at `0x80000000 + P`; the low 64 MB of VA is the image and
heap, a different mapping. Every page table -- and every DirectSound voice
buffer -- read the wrong bytes. The template now passes the window's base
(`XBOX_CONTIG_BASE`).

## Where the two titles stand

Measured with `scripts/run_and_report.py --seconds 60 --profile 100`, Release
builds, `RECOMP_VBLANK=1 RECOMP_AC97_READY=1`, no input:

| | Burnout 2 | TimeSplitters 2 |
|---|---|---|
| before this work | exit 0xC0000005 in 0.5-4 s, empty log | dashboard after 15 kernel calls |
| after | 29,900 functions, still climbing at 60 s, 14 files, front end reached | 1,137 functions, 20 files (front-end paks), menu music streaming, swaps at 60 Hz |
| overrides in `recomp_manual.c` | 0 | 0 |

TimeSplitters 2 settles at 1,137 functions with no input; it is presumably at
its title screen waiting for a button.

## What is next, in order

1. **Vertex programs through the direct API (HLE).** TimeSplitters 2 draws
   through `DrawVertices` from stream 0 (752 calls in 60 s), and the shadow
   renderer skips every one as "unknown shader". Its XDK loads shader
   microcode with `D3DDevice_LoadVertexShaderProgram` (4 calls) and selects
   it with `D3DDevice_SelectVertexShaderDirect` (160 calls) plus
   `SelectVertexShader`, never `CreateVertexShader`, so `src/hle` never
   learns the program. The handles it selects (`0x001EC3D1`) point at static
   shader objects in the D3D section's BSS, filled at runtime. Replacing
   `LoadVertexShaderProgram` (microcode at a slot) and
   `SelectVertexShaderDirect` (declaration plus slot) by name is the general
   fix; both are named in the 4721 symbols.
2. **Pixel shaders.** `SetPixelShader(0x001E9BCC)` "is not a shader object
   with a definition at +0x0C": the 4721 object layout differs from 5344's.
   `RECOMP_HLE_D3D8_PS_PROBE=1` dumps the object.
3. **Input.** Nothing has pressed a button yet; XAPI input is replaced by name
   and reports a pad on port 0, so the next run should drive the menu
   (`RECOMP_FAKE_INPUT`).
4. **Junk in the lift.** 1,124 unimplemented-instruction comments in the
   TimeSplitters lift, 1,010 of them inside XGRPH, are data the disassembler
   took for code (`outsd`, `insb`, `popal`, `arpl`). Harmless unless executed;
   the function-discovery item in `CLAUDE.md`.
5. **Seeds.** `config/seeds/4553000A.json` holds the thread starts and
   indirect-call targets the first runs observed; `tools.seed_from_log`
   maintains it.

## Working on two titles

    titles/<name>/                    the project: CMakeLists, main.c, recomp_manual.c   (committed)
    games/<title>/                    default.xbe + data + *_analysis.json + *_xdk_symbols.json (ignored)
    games/_pipeline/<name>/out/       disasm/ func_id/ recomp/ for that title           (ignored)

    py -3 scripts/recompile.py "games/<title>/default.xbe" \
        --work-dir games/_pipeline/<name>/out --project titles/<name> --trace-all-entries
    cmake -S titles/<name> -B titles/<name>/build -G "Visual Studio 16 2019" -A x64 -DRECOMP_ABI_CHECK=ON
    cmake --build titles/<name>/build --config Release -- -m
    RECOMP_VBLANK=1 RECOMP_AC97_READY=1 py -3 scripts/run_and_report.py \
        titles/<name>/build/Release/<name>_recomp.exe --seconds 60 --profile 100

`--work-dir` exists because every stage defaulted to one shared
`tools/*/output`, and the recompiler's guard against mixing titles compares
file names only -- every retail disc is `default.xbe`. A title's `main.c` is
regenerated from the template with `scripts/regen_title_main.py`, so a
template fix reaches every title. Keep nothing but the XBE, its data and its
two JSONs in `games/<title>/`: `extract-xiso -c` packs the whole folder into
the disc image xemu boots.
