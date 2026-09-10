# NV2A vertex program encoding — what is derived, and what is not

`src/d3d/d3d8_vsh.c` parses vertex microcode into `NV2AVshProgram`. Its field
table (`VSH_FIELD_*`) is **wrong**, in a way that cannot be partially right:
several fields overlap, so a destination register is decoded partly from a
write mask and an output selector's low bit is the final-instruction flag.
The consequence is that `d3d8_vsh_execute` writes nothing at all —
`out_written == 0` for every vertex.

Nothing in this repository documents the real encoding, and
`docs/technical/nv2a-shaders.md` points at a microcode translator in
`nv2a_pgraph_d3d11.c` that does not exist. So the table below was derived from
Burnout 2's own microcode rather than from recollection, and each row says
what makes it believable. **Rows marked `UNRESOLVED` must not be treated as
known.**

## The sample

`RECOMP_VP_DUMP=<path>` writes the uploaded program. Burnout 2's frontend
uploads twelve instructions, and `dword0` is zero in all of them — which is
the first hint the field table is displaced, since it places the opcodes
there.

```
insn   0 00000000 0020001B 0836106C 2F100FF8
insn   1 00000000 0420061B 083613FC 5011F818
insn   2 00000000 0400001B 083613FC 2070F82C
insn   3 00000000 0240081B 1436186C 2F20F824
insn   4 00000000 0060201B 2436106C 3070F800
insn   5 00000000 00200200 0836106C 2070F830
insn   6 00000000 00200E1B 0836106C 2070F838
insn   7 00000000 0020101B 0836106C 2070F840
insn   8 00000000 0020121B 0836106C 2070F848
insn   9 00000000 0020141B 0836106C 2070F850
insn  10 00000000 0020161B 0836106C 2070F858
insn  11 00000000 0020181B 0836106C 2070F861
```

## Derived, with evidence

| Field | Position | Why it is believed |
|---|---|---|
| `FINAL` | dword3 bit 0 | Set on instruction 11 and on no other. Exactly one final bit is what a valid program has. |
| `OUT_ADDRESS` | dword3 bits 3-10 | Decodes to 7, 8, 9, 10, 11, 12 across instructions 6-11, which are `oB0`, `oB1`, `oT0`, `oT1`, `oT2`, `oT3` in the output enum's own order. Instruction 0 gives `0xFF`, which the header already defines as "no output". |
| `CONST` | dword1 bits 9-16 | With this offset the program reads `c3` into `oD0`, `c4` into `oD1`, `c7` into `oB0`, `c8` into `oB1`, and `c9`-`c12` into `oT0`-`oT3`. The output enum numbers those outputs 3, 4, 7, 8, 9-12. Six consecutive `MOV o<N>, c<N>` is not a coincidence. At the previously assumed offset every source read `c0`. |
| `MAC` opcode | dword1 bits 21-24 | Yields MOV for the ten output initialisers, MUL at 3 and ADD at 4 — the only two instructions that compute anything. |
| `ILU` opcode | dword1 bits 25-27 | Yields NOP almost everywhere and RCP at instructions 1-2, which is what a perspective divide looks like. Three bits, not four: the enum has eight entries. |
| A swizzle | dword1 bits 0-7 (w,z,y,x at 0,2,4,6) | Instruction 0's low byte is `0x1B` = `00 01 10 11`, which reads as the identity swizzle `xyzw`. A wrong offset would make the commonest swizzle in any program look like something else. |
| A source bank | dword2 bits 26-27 | Const for the ten initialisers, input for instructions 3 and 4 — the two that read vertex data. |
| A source index | dword2 bits 28-31 | `v1` at instruction 3 and `v2` at instruction 4, consistent with the bank above. |
| Destination masks and mux | dword3: MAC mask 24-27, temp index 20-23, ILU mask 16-19, output mask 12-15, ORB 11, MUX 2 | Instruction 0 reads as `MOV R1, c0` with no output write; instruction 11 as `MOV oT3, c12` with no temp write. Both are coherent, and they are the two ends of the program. |

## Resolved, authoritatively

The derivation above is correct, and the two fields it could not reach are now
known. `abaire/nv2a_vsh_asm` is an assembler and disassembler for this exact
instruction set; its `vsh_instruction.py` defines the encoding as ctypes
bitfields, LSB-first per dword, and it agrees with every field derived above.

Running its disassembler over Burnout 2's twelve instructions gives ground
truth, and the result is the canonical Xbox pass-through shader:

```
 0  MOV R1.xyzw, v0
 1  MOV oD0.xyzw, v3      + RCP R1.w, R1.w
 2  RCP oFog.xyzw, v0.w
 3  MUL R2.xyzw, R1, c[0] + MOV oD1.xyzw, v4
 4  ADD oPos.xyzw, R2, c[1]
 5  MOV oPts.xyzw, v1.x
 6  MOV oB0.xyzw, v7          9  MOV oT1.xyzw, v10
 7  MOV oB1.xyzw, v8         10  MOV oT2.xyzw, v11
 8  MOV oT0.xyzw, v9         11  MOV oT3.xyzw, v12
```

Every attribute passes to the output with the matching number, which is what
confirmed the INPUT field: my own derivation had read those bits as CONST and
produced a plausible-looking `MOV o<N>, c<N>` instead. The one wrong call in
nine, and it looked right.

`oPos = v0 * c[0] + c[1]`, and the constants are `c[0] = (1, 1, 16777215, 1)`
and `c[1] = (0.53125, 0.53125, 0, 0)` -- a half-pixel bias on coordinates that
are already in window space. So for this shader the transform is identity, the
raw attribute is the correct position, and executing the program cannot change
where anything lands.

### The full layout

Absolute bit offsets for `vsh_extract()`, where dword N starts at bit 32*N.
dword0 is unused.

| dword1 | dword2 | dword3 |
|---|---|---|
| A_SWZ_W 32, Z 34, Y 36, X 38 | C_TEMP_HIGH 64 (2) | FINAL 96 |
| A_NEG 40 | C_SWZ_W 66, Z 68, Y 70, X 72 | A0X 97 |
| INPUT 41 (4) | C_NEG 74 | OUT_MUX 98 |
| CONST 45 (8) | B_MUX 75 (2), B_TEMP 77 (4) | OUT_ADDRESS 99 (8) |
| MAC 53 (4) | B_SWZ_W 81, Z 83, Y 85, X 87 | OUT_ORB 107 |
| ILU 57 (3) | B_NEG 89 | OUT_O_MASK 108 (4) |
| | A_MUX 90 (2), A_TEMP 92 (4) | OUT_ILU_MASK 112 (4) |
| | | OUT_TEMP 116 (4) |
| | | OUT_MAC_MASK 120 (4) |
| | | C_MUX 124 (2), C_TEMP_LOW 126 (2) |

Three asymmetries, each of which is a bug in the current parser:

- **A source bank is 1 = temp, 2 = input, 3 = const.** Zero is not a bank. The
  parser maps the raw value onto an enum that starts at zero, so every operand
  comes out one bank wrong.
- **The ILU opcode is three bits**, read as four, which makes unknown opcodes
  reachable.
- **MAC and ILU share one destination temp index** and have a write mask each,
  and there is a *single* output-register write whose source OUT_MUX selects.
  The parser gives each unit its own output register, which does not exist,
  and it carries one mask where there are two.
- **Source C's temp index is split** across dword2 bits 64-65 and dword3 bits
  126-127, so it cannot be read as one field.

A rewrite against this table was attempted and reverted: the scripted edit
removed declarations it should not have touched, and since the transform is
identity for the only shader this title currently uploads, the change cannot
affect the image. It is worth doing before any title that draws real 3D, and
the disassembler above is the oracle to test it against -- these twelve
instructions must decode identically.

## Previously unresolved

**Source B and source C field positions.** Only one instruction in this
program uses B (the `MUL` at 3) and four use C, so the sample does not
constrain them: a search for two-bit fields whose value is always a legal
bank (0, 1 or 2) leaves sixteen candidates. Fitting a position to four data
points is how a plausible wrong answer gets adopted.

**Whether an input register index is shared per instruction or per source.**
The hardware is documented elsewhere as having one `v#` per instruction, but
here the per-source index field (dword2 bits 28-31) already produces the right
answer, and this program never reads two different inputs in one slot. The two
readings are indistinguishable on this sample.

## How the remaining question was going to be resolved

(Kept for the record; the reference above settled it.) Capture a program that
transforms world geometry. It will contain a four-
instruction `DP4` chain against consecutive constants — the matrix multiply —
and a `DP4` uses A and B together with known operand roles, which pins both
fields immediately. Burnout 2 uploads such a program once it draws something
other than its frontend; the frontend shader above is close to a pass-through,
which is also why its vertices arrive already in pixels.

Until then `fetch_position()` in `nv2a_pb_exec.c` falls back to the raw
attribute whenever the program writes no position, and counts every time it
does so.
