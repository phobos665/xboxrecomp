/**
 * NV2A Vertex Shader Microcode to HLSL Translator - Implementation
 *
 * Translates NV2A 128-bit vertex shader microcode instructions into
 * HLSL vertex shader source, compiles them, and caches the results.
 *
 * The translation pipeline is:
 *   1. Parse: nv2a_vsh_parse(), in src/kernel/nv2a_vsh.c
 *   2. Analyze: determine which input registers (v0-v15) are read
 *   3. Generate HLSL: emit HLSL code mapping NV2A ops to HLSL intrinsics
 *   4. Compile: D3DCompile -> ID3D11VertexShader
 *   5. Cache: hash microcode -> reuse compiled shader on subsequent draws
 *
 * The generated HLSL uses:
 *   - cbuffer at b1: 192 float4 constants (c0-c191)
 *   - Input semantics: ATTR0-ATTR15 mapped to v0-v15
 *   - Output semantics: SV_POSITION, COLOR0/1, TEXCOORD0-3, FOG, PSIZE
 */

#include "d3d8_internal.h"
#include <float.h>
#include "d3d8_vsh.h"
#include <d3dcompiler.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <math.h>

#pragma comment(lib, "d3dcompiler.lib")

/* ================================================================
 * Module State
 * ================================================================ */

/* Stored shader programs */
static NV2AVshSlot g_vsh_slots[NV2A_VS_MAX_SLOTS];
static int g_vsh_slot_count = 0;

/* Constant registers (192 float4) */
static NV2AVSConstants g_vsh_constants;
static BOOL g_vsh_constants_dirty = TRUE;

/* D3D11 constant buffer for VS constants */
static ID3D11Buffer *g_vsh_cb = NULL;

/* Shader cache: maps microcode hash to compiled shader + input layout */
typedef struct {
    uint32_t            hash;
    int                 in_use;
    ID3D11VertexShader *vs;
    ID3DBlob           *vs_blob;      /* Bytecode for input layout creation */
    ID3D11InputLayout  *layouts[16];  /* Cached layouts per input mask subset */
    uint32_t            layout_masks[16];  /* inputs_read | (texcoord-field<<16) */
    int                 layout_count;
    uint16_t            inputs_read;  /* Which v registers are read */
} VshCacheEntry;

static VshCacheEntry g_vsh_cache[NV2A_VS_CACHE_SIZE];

/* ================================================================
 * Hash Function (FNV-1a)
 * ================================================================ */

static uint32_t fnv1a_hash(const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t h = 0x811c9dc5;
    size_t i;
    for (i = 0; i < len; i++) {
        h ^= p[i];
        h *= 0x01000193;
    }
    return h;
}

/* ================================================================
 * HLSL Code Generator
 * ================================================================ */

/* String buffer helper */
typedef struct {
    char *buf;
    int   pos;
    int   size;
} StrBuf;

static void sb_init(StrBuf *sb, char *buf, int size)
{
    sb->buf  = buf;
    sb->pos  = 0;
    sb->size = size;
    if (size > 0) buf[0] = '\0';
}

static void sb_append(StrBuf *sb, const char *fmt, ...)
{
    va_list ap;
    int remaining;
    if (sb->pos >= sb->size - 1) return;
    remaining = sb->size - sb->pos;
    va_start(ap, fmt);
    int n = vsnprintf(sb->buf + sb->pos, remaining, fmt, ap);
    va_end(ap);
    if (n > 0 && n < remaining)
        sb->pos += n;
    else if (n >= remaining)
        sb->pos = sb->size - 1;
}

/* Component name table */
static const char g_comp_names[] = "xyzw";

/**
 * Emit a swizzle suffix.
 * If the swizzle is identity (.xyzw), emit nothing (saves readability).
 */
static void emit_swizzle(StrBuf *sb, const NV2AVshSwizzle *swz)
{
    /* Check for identity swizzle */
    if (swz->x == 0 && swz->y == 1 && swz->z == 2 && swz->w == 3)
        return;

    sb_append(sb, ".%c%c%c%c",
              g_comp_names[swz->x & 3],
              g_comp_names[swz->y & 3],
              g_comp_names[swz->z & 3],
              g_comp_names[swz->w & 3]);
}

/**
 * Emit a scalar swizzle for ILU ops that replicate a single component.
 * Uses .x/.y/.z/.w for the selected component.
 */
static void emit_scalar_swizzle(StrBuf *sb, const NV2AVshSwizzle *swz)
{
    /* ILU operations use only one component; the swizzle X field selects it */
    sb_append(sb, ".%c", g_comp_names[swz->x & 3]);
}

/**
 * Emit a source operand reference.
 *
 * Handles register bank selection, swizzle, negate, and relative addressing.
 */
static void emit_source(StrBuf *sb, const NV2AVshSrcOperand *src, int scalar)
{
    if (src->negate)
        sb_append(sb, "(-");

    switch (src->reg_type) {
    case NV2A_VSH_REG_TEMP:
        /* R12 is the oPos alias. R13-R15 do not exist: a read of one is
         * zero, as in the CPU interpreter, instead of an undeclared name that
         * fails the whole shader's compile. */
        if (src->reg_index >= 0 && src->reg_index <= 12)
            sb_append(sb, "R%d", src->reg_index);
        else
            sb_append(sb, "float4(0,0,0,0)");
        break;
    case NV2A_VSH_REG_INPUT:
        sb_append(sb, "v%d", src->reg_index);
        break;
    case NV2A_VSH_REG_CONST:
        if (src->rel_addr)
            sb_append(sb, "c[a0 + %d]", src->reg_index);
        else
            sb_append(sb, "c[%d]", src->reg_index);
        break;
    default:
        sb_append(sb, "float4(0,0,0,0)");
        break;
    }

    if (scalar)
        emit_scalar_swizzle(sb, &src->swizzle);
    else
        emit_swizzle(sb, &src->swizzle);

    if (src->negate)
        sb_append(sb, ")");
}

/**
 * Emit a write mask suffix (.xyzw subset).
 * The mask is encoded as: bit3=x, bit2=y, bit1=z, bit0=w.
 */
static void emit_write_mask(StrBuf *sb, uint8_t mask)
{
    if (mask == 0xF) return; /* Full write, no mask needed */

    sb_append(sb, ".");
    if (mask & 0x8) sb_append(sb, "x");
    if (mask & 0x4) sb_append(sb, "y");
    if (mask & 0x2) sb_append(sb, "z");
    if (mask & 0x1) sb_append(sb, "w");
}

/**
 * Map NV2A output register enum to an HLSL variable name.
 */
static const char *output_reg_name(NV2AVshOutputReg reg)
{
    switch (reg) {
    case NV2A_VSH_OUT_POS:  return "oPos";
    case NV2A_VSH_OUT_D0:   return "oD0";
    case NV2A_VSH_OUT_D1:   return "oD1";
    case NV2A_VSH_OUT_FOG:  return "oFog";
    case NV2A_VSH_OUT_PTS:  return "oPts";
    case NV2A_VSH_OUT_B0:   return "oB0";
    case NV2A_VSH_OUT_B1:   return "oB1";
    case NV2A_VSH_OUT_T0:   return "oT0";
    case NV2A_VSH_OUT_T1:   return "oT1";
    case NV2A_VSH_OUT_T2:   return "oT2";
    case NV2A_VSH_OUT_T3:   return "oT3";
    default:                return NULL;
    }
}

/**
 * Emit a destination assignment (temp and/or output register write).
 *
 * The NV2A can write to both a temp register and an output register
 * simultaneously from the same operation. We emit two assignments
 * when both are active.
 *
 * @param prefix  The destination: temp register name
 * @param dst     The destination operand
 * @param rhs     The HLSL expression to assign (right-hand side)
 */
static void emit_dest_assign(StrBuf *sb, const NV2AVshDstOperand *dst,
                              const char *rhs)
{
    uint8_t omask = nv2a_vsh_output_write_mask(dst);

    /* rhs is a local computed once for the slot, so the order of these two
     * writes does not change the value. It did while rhs was the expression
     * itself: the temp write came first and the output then re-evaluated the
     * expression against the new temp. */
    if (dst->output_reg != NV2A_VSH_OUT_NONE && omask != 0) {
        const char *name = output_reg_name(dst->output_reg);
        if (name) {
            sb_append(sb, "        %s", name);
            emit_write_mask(sb, omask);
            sb_append(sb, " = (%s)", rhs);
            emit_write_mask(sb, omask);
            sb_append(sb, ";\n");
        }
    }

    /* R13-R15 do not exist; the interpreter ignores writes to them too. */
    if (dst->temp_reg >= 0 && dst->temp_reg <= 12 && dst->write_mask != 0) {
        sb_append(sb, "        R%d", dst->temp_reg);
        emit_write_mask(sb, dst->write_mask);
        sb_append(sb, " = (%s)", rhs);
        emit_write_mask(sb, dst->write_mask);
        sb_append(sb, ";\n");
    }
}

/**
 * Emit the HLSL for one MAC operation.
 */
static void emit_mac_op(StrBuf *sb, const NV2AVshInstruction *inst)
{
    StrBuf expr;
    char expr_buf[512];
    sb_init(&expr, expr_buf, sizeof(expr_buf));

    switch (inst->mac_op) {
    case NV2A_VSH_MAC_NOP:
        return;

    case NV2A_VSH_MAC_MOV:
        /* dst = A */
        emit_source(&expr, &inst->mac_src[0], 0);
        break;

    case NV2A_VSH_MAC_MUL:
        /* dst = A * B */
        sb_append(&expr, "(");
        emit_source(&expr, &inst->mac_src[0], 0);
        sb_append(&expr, " * ");
        emit_source(&expr, &inst->mac_src[1], 0);
        sb_append(&expr, ")");
        break;

    case NV2A_VSH_MAC_ADD:
        /* dst = A + C */
        sb_append(&expr, "(");
        emit_source(&expr, &inst->mac_src[0], 0);
        sb_append(&expr, " + ");
        emit_source(&expr, &inst->mac_src[2], 0);
        sb_append(&expr, ")");
        break;

    case NV2A_VSH_MAC_MAD:
        /* dst = A * B + C */
        sb_append(&expr, "(");
        emit_source(&expr, &inst->mac_src[0], 0);
        sb_append(&expr, " * ");
        emit_source(&expr, &inst->mac_src[1], 0);
        sb_append(&expr, " + ");
        emit_source(&expr, &inst->mac_src[2], 0);
        sb_append(&expr, ")");
        break;

    case NV2A_VSH_MAC_DP3:
        /* dst.xyzw = dot(A.xyz, B.xyz) replicated */
        sb_append(&expr, "dot(");
        emit_source(&expr, &inst->mac_src[0], 0);
        sb_append(&expr, ".xyz, ");
        emit_source(&expr, &inst->mac_src[1], 0);
        sb_append(&expr, ".xyz).xxxx");
        break;

    case NV2A_VSH_MAC_DPH:
        /* dst = dot(float4(A.xyz, 1.0), B) */
        sb_append(&expr, "dot(float4(");
        emit_source(&expr, &inst->mac_src[0], 0);
        sb_append(&expr, ".xyz, 1.0), ");
        emit_source(&expr, &inst->mac_src[1], 0);
        sb_append(&expr, ").xxxx");
        break;

    case NV2A_VSH_MAC_DP4:
        /* dst.xyzw = dot(A, B) replicated */
        sb_append(&expr, "dot(");
        emit_source(&expr, &inst->mac_src[0], 0);
        sb_append(&expr, ", ");
        emit_source(&expr, &inst->mac_src[1], 0);
        sb_append(&expr, ").xxxx");
        break;

    case NV2A_VSH_MAC_DST:
        /* dst = float4(1.0, A.y * B.y, A.z, B.w) */
        sb_append(&expr, "float4(1.0, ");
        emit_source(&expr, &inst->mac_src[0], 0);
        sb_append(&expr, ".y * ");
        emit_source(&expr, &inst->mac_src[1], 0);
        sb_append(&expr, ".y, ");
        emit_source(&expr, &inst->mac_src[0], 0);
        sb_append(&expr, ".z, ");
        emit_source(&expr, &inst->mac_src[1], 0);
        sb_append(&expr, ".w)");
        break;

    case NV2A_VSH_MAC_MIN:
        sb_append(&expr, "min(");
        emit_source(&expr, &inst->mac_src[0], 0);
        sb_append(&expr, ", ");
        emit_source(&expr, &inst->mac_src[1], 0);
        sb_append(&expr, ")");
        break;

    case NV2A_VSH_MAC_MAX:
        sb_append(&expr, "max(");
        emit_source(&expr, &inst->mac_src[0], 0);
        sb_append(&expr, ", ");
        emit_source(&expr, &inst->mac_src[1], 0);
        sb_append(&expr, ")");
        break;

    case NV2A_VSH_MAC_SLT:
        /* dst = (A < B) ? 1.0 : 0.0
         * SLT is the complement of SGE: slt(a,b) = 1 - step(b, a)
         * Equivalent to: step(a, b) where a < b yields 1
         * Using explicit form for clarity: */
        sb_append(&expr, "(1.0 - step(");
        emit_source(&expr, &inst->mac_src[1], 0);
        sb_append(&expr, ", ");
        emit_source(&expr, &inst->mac_src[0], 0);
        sb_append(&expr, "))");
        break;

    case NV2A_VSH_MAC_SGE:
        /* dst = (A >= B) ? 1.0 : 0.0
         * step(edge, x) returns 1 if x >= edge, 0 otherwise */
        sb_append(&expr, "step(");
        emit_source(&expr, &inst->mac_src[1], 0);
        sb_append(&expr, ", ");
        emit_source(&expr, &inst->mac_src[0], 0);
        sb_append(&expr, ")");
        break;

    case NV2A_VSH_MAC_ARL:
        /* a0 = floor(A.x). Held in a local and stored after the ILU has read
         * its source, like every other write in the slot. */
        sb_append(sb, "        int _a0 = (int)floor(");
        emit_source(sb, &inst->mac_src[0], 0);
        sb_append(sb, ".x);\n");
        return;

    default:
        return;
    }

    sb_append(sb, "        float4 _mac = %s;\n", expr_buf);
}

/**
 * Emit the HLSL for one ILU operation.
 */
static void emit_ilu_op(StrBuf *sb, const NV2AVshInstruction *inst)
{
    StrBuf expr;
    char expr_buf[512];
    sb_init(&expr, expr_buf, sizeof(expr_buf));

    switch (inst->ilu_op) {
    case NV2A_VSH_ILU_NOP:
        return;

    case NV2A_VSH_ILU_MOV:
        /* dst = C */
        emit_source(&expr, &inst->ilu_src, 0);
        break;

    case NV2A_VSH_ILU_RCP:
        /* dst = (1.0 / C.x).xxxx */
        sb_append(&expr, "(1.0 / ");
        emit_source(&expr, &inst->ilu_src, 1);
        sb_append(&expr, ").xxxx");
        break;

    case NV2A_VSH_ILU_RCC:
        /* dst = sign(1/C.x) * clamp(|1/C.x|, 5.42101e-20, 1.8446744e+19),
         * replicated -- the same asymmetric clamp nv2a_vsh_execute() applies.
         * The sign is taken as ">= 0", not sign(): at C.x = +/-inf, 1/x is a
         * signed zero, which sign() makes 0 and the interpreter treats as
         * positive. 1/0 is +/-inf and clamps to the upper bound. */
        sb_append(&expr, "(((1.0 / ");
        emit_source(&expr, &inst->ilu_src, 1);
        sb_append(&expr, ") >= 0.0 ? 1.0 : -1.0) * clamp(abs(1.0 / ");
        emit_source(&expr, &inst->ilu_src, 1);
        sb_append(&expr, "), 5.42101e-20, 1.8446744e+19)).xxxx");
        break;

    case NV2A_VSH_ILU_RSQ:
        /* dst = (1.0 / sqrt(abs(C.x))).xxxx */
        sb_append(&expr, "rsqrt(abs(");
        emit_source(&expr, &inst->ilu_src, 1);
        sb_append(&expr, ")).xxxx");
        break;

    case NV2A_VSH_ILU_EXP:
        /* dst = exp2(C.x).xxxx */
        sb_append(&expr, "exp2(");
        emit_source(&expr, &inst->ilu_src, 1);
        sb_append(&expr, ").xxxx");
        break;

    case NV2A_VSH_ILU_LOG: {
        /* dst = log2(abs(C.x)).xxxx
         * Guard against log2(0) which is -inf on NV2A -> clamp to large negative */
        sb_append(&expr, "log2(max(abs(");
        emit_source(&expr, &inst->ilu_src, 1);
        sb_append(&expr, "), 1.175494e-38)).xxxx");
        break;
    }

    case NV2A_VSH_ILU_LIT: {
        /* NV2A LIT instruction:
         *   dst.x = 1.0
         *   dst.y = max(src.x, 0.0)
         *   dst.z = (src.x > 0) ? pow(max(src.y, 0), clamp(src.w, -128, 128)) : 0
         *   dst.w = 1.0
         *
         * We emit a helper call. The lit() HLSL intrinsic has similar but
         * not identical semantics, so we use an inline expansion. */
        sb_append(&expr, "float4(1.0, max(");
        emit_source(&expr, &inst->ilu_src, 0);
        sb_append(&expr, ".x, 0.0), (");
        emit_source(&expr, &inst->ilu_src, 0);
        sb_append(&expr, ".x > 0.0) ? exp2(clamp(");
        emit_source(&expr, &inst->ilu_src, 0);
        sb_append(&expr, ".w, -128.0, 128.0) * log2(max(");
        emit_source(&expr, &inst->ilu_src, 0);
        sb_append(&expr, ".y, 0.0) + 1e-30)) : 0.0, 1.0)");
        break;
    }

    default:
        return;
    }

    sb_append(sb, "        float4 _ilu = %s;\n", expr_buf);
}

/**
 * Map NV2A input register index to a D3D11 input semantic.
 *
 * Xbox NV2A vertex shader input registers map to vertex attributes:
 *   v0  = Position
 *   v1  = Blend weight
 *   v2  = Normal
 *   v3  = Diffuse color
 *   v4  = Specular color
 *   v5  = Fog coordinate
 *   v6  = Point size / back diffuse
 *   v7  = Back specular
 *   v8  = Texture coord 0
 *   v9  = Texture coord 1
 *   v10 = Texture coord 2
 *   v11 = Texture coord 3
 *   v12-v15 = Additional attributes
 *
 * We use generic ATTR semantics so the input layout can match any
 * vertex buffer format at bind time.
 */
static const char *input_semantic_name(int reg_index)
{
    /* All inputs use the generic TEXCOORD semantic with unique indices
     * to avoid mismatches. The input layout will map them correctly. */
    (void)reg_index;
    return "ATTR";
}

int d3d8_vsh_generate_hlsl(const NV2AVshProgram *program,
                            char *buf, int bufsize)
{
    StrBuf sb;
    int i;
    uint16_t inputs = program->inputs_read;

    sb_init(&sb, buf, bufsize);

    /* Constant buffer: 192 float4 constants */
    sb_append(&sb,
        "/* Auto-generated NV2A vertex shader */\n"
        "\n"
        "cbuffer VSH_Constants : register(b1) {\n"
        "    float4 c[%d];\n"
        "};\n"
        "\n", NV2A_VS_MAX_CONSTANTS);

    /* Input structure - only declare used inputs */
    sb_append(&sb, "struct VS_IN {\n");
    for (i = 0; i < NV2A_VS_MAX_INPUTS; i++) {
        if (inputs & (1u << i)) {
            sb_append(&sb, "    float4 v%d : ATTR%d;\n", i, i);
        }
    }
    sb_append(&sb, "};\n\n");

    /* Output structure */
    sb_append(&sb,
        "struct VS_OUT {\n"
        "    float4 oPos : SV_POSITION;\n"
        "    float4 oD0  : COLOR0;\n"
        "    float4 oD1  : COLOR1;\n"
        "    float4 oT0  : TEXCOORD0;\n"
        "    float4 oT1  : TEXCOORD1;\n"
        "    float4 oT2  : TEXCOORD2;\n"
        "    float4 oT3  : TEXCOORD3;\n"
        "    float  oFog : FOG;\n"
        "    float  oPts : PSIZE;\n"
        "    float4 oB0  : TEXCOORD4;\n"
        "    float4 oB1  : TEXCOORD5;\n"
        "};\n\n");

    /* Main function */
    sb_append(&sb, "VS_OUT main(VS_IN input) {\n");

    /* Declare temporary registers R0-R12 */
    sb_append(&sb, "    /* Temporary registers */\n");
    for (i = 0; i <= 12; i++) {
        sb_append(&sb, "    float4 R%d = float4(0,0,0,0);\n", i);
    }

    /* Address register */
    sb_append(&sb, "    int a0 = 0;\n\n");

    /* Alias input registers for readability */
    sb_append(&sb, "    /* Input register aliases */\n");
    for (i = 0; i < NV2A_VS_MAX_INPUTS; i++) {
        if (inputs & (1u << i)) {
            sb_append(&sb, "    float4 v%d = input.v%d;\n", i, i);
        }
    }
    sb_append(&sb, "\n");

    /* Output register variables */
    sb_append(&sb,
        "    /* Output registers (initialized to zero) */\n"
        "    float4 oPos = float4(0,0,0,1);\n"
        "    float4 oD0  = float4(0,0,0,1);\n"
        "    float4 oD1  = float4(0,0,0,1);\n"
        "    float4 oFog = float4(0,0,0,0);\n"
        "    float4 oPts = float4(0,0,0,0);\n"
        "    float4 oB0  = float4(0,0,0,0);\n"
        "    float4 oB1  = float4(0,0,0,0);\n"
        "    float4 oT0  = float4(0,0,0,0);\n"
        "    float4 oT1  = float4(0,0,0,0);\n"
        "    float4 oT2  = float4(0,0,0,0);\n"
        "    float4 oT3  = float4(0,0,0,0);\n"
        "\n");

    /* R12 is aliased to oPos on NV2A */
    sb_append(&sb, "    /* R12 is aliased to oPos */\n");
    sb_append(&sb, "    #define R12 oPos\n\n");

    /* Emit instructions */
    sb_append(&sb, "    /* --- Program body (%d instructions) --- */\n",
              program->length);

    for (i = 0; i < program->length; i++) {
        const NV2AVshInstruction *inst = &program->insns[i];

        int mac_runs = inst->mac_op != NV2A_VSH_MAC_NOP
                    && (unsigned)inst->mac_op < NV2A_VSH_MAC_COUNT;
        int ilu_runs = inst->ilu_op != NV2A_VSH_ILU_NOP;

        sb_append(&sb, "\n    /* Instruction %d */\n    {\n", i);

        /* The two units run in parallel: both read the register file as it
         * was at the start of the slot. So every value is computed into a
         * local first -- _mac, _a0, _ilu -- and only then written. Emitting
         * the MAC write before evaluating the ILU let an ILU source read a
         * temp the MAC had just changed, which the CPU interpreter never did.
         * MAC opcodes 14 and 15 do not exist and emit nothing. */
        if (mac_runs)
            emit_mac_op(&sb, inst);
        if (ilu_runs)
            emit_ilu_op(&sb, inst);

        if (mac_runs && inst->mac_op == NV2A_VSH_MAC_ARL)
            sb_append(&sb, "        a0 = _a0;\n");
        else if (mac_runs)
            emit_dest_assign(&sb, &inst->mac_dst, "_mac");
        if (ilu_runs)
            emit_dest_assign(&sb, &inst->ilu_dst, "_ilu");

        sb_append(&sb, "    }\n");
    }

    /* Undo the R12 alias */
    sb_append(&sb, "\n    #undef R12\n\n");

    /* Populate output structure */
    sb_append(&sb,
        "    /* Write outputs */\n"
        "    VS_OUT o;\n"
        "    o.oPos = oPos;\n"
        "    o.oD0  = saturate(oD0);\n"  /* Colors clamped to [0,1] */
        "    o.oD1  = saturate(oD1);\n"
        "    o.oT0  = oT0;\n"
        "    o.oT1  = oT1;\n"
        "    o.oT2  = oT2;\n"
        "    o.oT3  = oT3;\n"
        "    o.oFog = oFog.x;\n"
        "    o.oPts = oPts.x;\n"
        "    o.oB0  = saturate(oB0);\n"
        "    o.oB1  = saturate(oB1);\n"
        "    return o;\n"
        "}\n");

    return sb.pos;
}

/* ================================================================
 * Input Layout Management
 *
 * When using a programmable VS, we need an input layout that matches
 * the shader's declared inputs. The layout maps vertex buffer elements
 * to the ATTR# semantics declared in the generated HLSL.
 *
 * The mapping from NV2A v# registers to vertex data depends on the
 * game's vertex stream setup. We use a simple mapping:
 *
 *   v0  -> ATTR0  -> POSITION (float4, offset 0)
 *   v1  -> ATTR1  -> BLENDWEIGHT (float4)
 *   v2  -> ATTR2  -> NORMAL (float4)
 *   v3  -> ATTR3  -> DIFFUSE (float4 / D3DCOLOR)
 *   v4  -> ATTR4  -> SPECULAR (float4 / D3DCOLOR)
 *   v5  -> ATTR5  -> FOG (float4)
 *   v6  -> ATTR6  -> POINTSIZE / BACKDIFFUSE (float4)
 *   v7  -> ATTR7  -> BACKSPECULAR (float4)
 *   v8  -> ATTR8  -> TEXCOORD0 (float4)
 *   v9  -> ATTR9  -> TEXCOORD1 (float4)
 *   v10 -> ATTR10 -> TEXCOORD2 (float4)
 *   v11 -> ATTR11 -> TEXCOORD3 (float4)
 *   v12-v15 -> ATTR12-15 -> additional
 *
 * The actual format (float2/3/4, D3DCOLOR, etc.) is determined at
 * draw time from the active stream source FVF/stride. For now, we
 * create a layout assuming the standard Xbox vertex attribute mapping.
 * ================================================================ */

/**
 * Default format for each input register.
 * This is the common Xbox convention; games may vary.
 * Texcoord registers 8-11 use the FVF-declared size (float1/2/3/4).
 */
static UINT vsh_fvf_texcoord_size(DWORD fvf, UINT t)
{
    DWORD field = (fvf >> (16 + t * 2)) & 0x3;
    return field == 0 ? 2 : field;
}

static DXGI_FORMAT default_input_format(int vreg, DWORD fvf)
{
    switch (vreg) {
    case 0:  return DXGI_FORMAT_R32G32B32_FLOAT;    /* Position (xyz) */
    case 1:  return DXGI_FORMAT_R32G32B32A32_FLOAT;  /* Blend weights */
    case 2:  return DXGI_FORMAT_R32G32B32_FLOAT;     /* Normal */
    case 3:  return DXGI_FORMAT_R8G8B8A8_UNORM;      /* Diffuse (D3DCOLOR) */
    case 4:  return DXGI_FORMAT_R8G8B8A8_UNORM;      /* Specular (D3DCOLOR) */
    case 5:  return DXGI_FORMAT_R32_FLOAT;            /* Fog */
    case 6:  return DXGI_FORMAT_R32_FLOAT;            /* Point size */
    case 7:  return DXGI_FORMAT_R8G8B8A8_UNORM;      /* Back specular */
    case 8:  return vsh_fvf_texcoord_size(fvf, 0) == 1 ? DXGI_FORMAT_R32_FLOAT
                 : vsh_fvf_texcoord_size(fvf, 0) == 3 ? DXGI_FORMAT_R32G32B32_FLOAT
                 : vsh_fvf_texcoord_size(fvf, 0) == 4 ? DXGI_FORMAT_R32G32B32A32_FLOAT
                 : DXGI_FORMAT_R32G32_FLOAT;          /* Texcoord 0 */
    case 9:  return vsh_fvf_texcoord_size(fvf, 1) == 1 ? DXGI_FORMAT_R32_FLOAT
                 : vsh_fvf_texcoord_size(fvf, 1) == 3 ? DXGI_FORMAT_R32G32B32_FLOAT
                 : vsh_fvf_texcoord_size(fvf, 1) == 4 ? DXGI_FORMAT_R32G32B32A32_FLOAT
                 : DXGI_FORMAT_R32G32_FLOAT;          /* Texcoord 1 */
    case 10: return vsh_fvf_texcoord_size(fvf, 2) == 1 ? DXGI_FORMAT_R32_FLOAT
                 : vsh_fvf_texcoord_size(fvf, 2) == 3 ? DXGI_FORMAT_R32G32B32_FLOAT
                 : vsh_fvf_texcoord_size(fvf, 2) == 4 ? DXGI_FORMAT_R32G32B32A32_FLOAT
                 : DXGI_FORMAT_R32G32_FLOAT;          /* Texcoord 2 */
    case 11: return vsh_fvf_texcoord_size(fvf, 3) == 1 ? DXGI_FORMAT_R32_FLOAT
                 : vsh_fvf_texcoord_size(fvf, 3) == 3 ? DXGI_FORMAT_R32G32B32_FLOAT
                 : vsh_fvf_texcoord_size(fvf, 3) == 4 ? DXGI_FORMAT_R32G32B32A32_FLOAT
                 : DXGI_FORMAT_R32G32_FLOAT;          /* Texcoord 3 */
    default: return DXGI_FORMAT_R32G32B32A32_FLOAT;   /* Generic */
    }
}

static UINT input_format_size(DXGI_FORMAT fmt)
{
    switch (fmt) {
    case DXGI_FORMAT_R32_FLOAT:            return 4;
    case DXGI_FORMAT_R32G32_FLOAT:         return 8;
    case DXGI_FORMAT_R32G32B32_FLOAT:      return 12;
    case DXGI_FORMAT_R32G32B32A32_FLOAT:   return 16;
    case DXGI_FORMAT_R8G8B8A8_UNORM:       return 4;
    default:                                return 16;
    }
}

static ID3D11InputLayout *create_vsh_input_layout(
    uint16_t inputs_read, DWORD fvf, ID3DBlob *vs_blob)
{
    D3D11_INPUT_ELEMENT_DESC elems[NV2A_VS_MAX_INPUTS];
    UINT elem_count = 0;
    UINT offset = 0;
    ID3D11InputLayout *layout = NULL;
    HRESULT hr;
    int i;

    for (i = 0; i < NV2A_VS_MAX_INPUTS; i++) {
        if (!(inputs_read & (1u << i)))
            continue;

        DXGI_FORMAT fmt = default_input_format(i, fvf);

        elems[elem_count].SemanticName      = "ATTR";
        elems[elem_count].SemanticIndex      = (UINT)i;
        elems[elem_count].Format             = fmt;
        elems[elem_count].InputSlot          = 0;
        elems[elem_count].AlignedByteOffset  = offset;
        elems[elem_count].InputSlotClass     = D3D11_INPUT_PER_VERTEX_DATA;
        elems[elem_count].InstanceDataStepRate = 0;
        elem_count++;

        offset += input_format_size(fmt);
    }

    if (elem_count == 0) return NULL;

    hr = ID3D11Device_CreateInputLayout(
        d3d8_GetD3D11Device(),
        elems, elem_count,
        ID3D10Blob_GetBufferPointer(vs_blob),
        ID3D10Blob_GetBufferSize(vs_blob),
        &layout);

    if (FAILED(hr)) {
        fprintf(stderr, "D3D8 VSH: CreateInputLayout failed: 0x%08lX\n", hr);
        return NULL;
    }

    return layout;
}

/* ================================================================
 * Shader Compilation and Caching
 * ================================================================ */

static VshCacheEntry *cache_lookup(uint32_t hash)
{
    int idx = (int)(hash % NV2A_VS_CACHE_SIZE);
    int i;
    for (i = 0; i < NV2A_VS_CACHE_SIZE; i++) {
        int probe = (idx + i) % NV2A_VS_CACHE_SIZE;
        if (!g_vsh_cache[probe].in_use)
            return NULL;
        if (g_vsh_cache[probe].hash == hash)
            return &g_vsh_cache[probe];
    }
    return NULL;
}

static VshCacheEntry *cache_insert(uint32_t hash)
{
    int idx = (int)(hash % NV2A_VS_CACHE_SIZE);
    int i;

    /* Find an empty slot or reuse the probed slot */
    for (i = 0; i < NV2A_VS_CACHE_SIZE; i++) {
        int probe = (idx + i) % NV2A_VS_CACHE_SIZE;
        if (!g_vsh_cache[probe].in_use) {
            g_vsh_cache[probe].hash   = hash;
            g_vsh_cache[probe].in_use = 1;
            return &g_vsh_cache[probe];
        }
    }

    /* Cache full: evict the first probed entry */
    {
        VshCacheEntry *evict = &g_vsh_cache[idx];
        int j;

        if (evict->vs)
            ID3D11VertexShader_Release(evict->vs);
        if (evict->vs_blob)
            ID3D10Blob_Release(evict->vs_blob);
        for (j = 0; j < evict->layout_count; j++) {
            if (evict->layouts[j])
                ID3D11InputLayout_Release(evict->layouts[j]);
        }
        memset(evict, 0, sizeof(*evict));
        evict->hash   = hash;
        evict->in_use = 1;
        return evict;
    }
}

/**
 * Compile a vertex shader from microcode.
 *
 * Parses, generates HLSL, compiles, and caches the result.
 * Returns the cache entry (with compiled VS and blob).
 */
static VshCacheEntry *compile_shader(const DWORD *microcode, int num_insns,
                                      uint32_t hash)
{
    NV2AVshProgram program;
    char hlsl_buf[16384];  /* 16KB should be enough for any VS */
    int hlsl_len;
    ID3DBlob *code = NULL, *errors = NULL;
    HRESULT hr;
    VshCacheEntry *entry;

    /* Parse microcode */
    nv2a_vsh_parse((const uint32_t *)microcode, num_insns, &program);

    /* Generate HLSL */
    hlsl_len = d3d8_vsh_generate_hlsl(&program, hlsl_buf, sizeof(hlsl_buf));
    if (hlsl_len <= 0) {
        fprintf(stderr, "D3D8 VSH: HLSL generation failed\n");
        return NULL;
    }

    /* Compile HLSL to bytecode */
    hr = D3DCompile(hlsl_buf, (SIZE_T)hlsl_len, "nv2a_vsh",
                    NULL, NULL, "main", "vs_5_0",
                    D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                    &code, &errors);
    if (FAILED(hr)) {
        fprintf(stderr, "D3D8 VSH: Compile failed: %s\n",
                errors ? (char *)ID3D10Blob_GetBufferPointer(errors) : "unknown");
        fprintf(stderr, "--- Generated HLSL ---\n%s\n--- End ---\n", hlsl_buf);
        if (errors) ID3D10Blob_Release(errors);
        return NULL;
    }
    if (errors) ID3D10Blob_Release(errors);

    /* Insert into cache */
    entry = cache_insert(hash);
    if (!entry) {
        ID3D10Blob_Release(code);
        return NULL;
    }

    /* Create D3D11 vertex shader */
    hr = ID3D11Device_CreateVertexShader(
        d3d8_GetD3D11Device(),
        ID3D10Blob_GetBufferPointer(code),
        ID3D10Blob_GetBufferSize(code),
        NULL, &entry->vs);

    if (FAILED(hr)) {
        fprintf(stderr, "D3D8 VSH: CreateVertexShader failed: 0x%08lX\n", hr);
        ID3D10Blob_Release(code);
        entry->in_use = 0;
        return NULL;
    }

    entry->vs_blob     = code;
    entry->inputs_read = program.inputs_read;
    entry->layout_count = 0;

    fprintf(stderr, "D3D8 VSH: Compiled shader (hash 0x%08X, %d insns, inputs 0x%04X)\n",
            hash, program.length, program.inputs_read);

    return entry;
}

/**
 * Get the input layout for a cache entry.
 * Creates and caches the layout on first request per (input mask, texcoord sizes).
 */
static ID3D11InputLayout *get_cached_layout(VshCacheEntry *entry, DWORD fvf)
{
    uint16_t mask = entry->inputs_read;
    uint32_t key = (uint32_t)mask | (((uint32_t)(fvf >> 16) & 0xFFFF) << 16);
    int i;

    /* Check if we already created a layout for this mask+texcoord layout */
    for (i = 0; i < entry->layout_count; i++) {
        if (entry->layout_masks[i] == key)
            return entry->layouts[i];
    }

    /* Create new layout */
    if (entry->layout_count >= 16)
        return entry->layouts[0]; /* Fallback to first */

    ID3D11InputLayout *layout = create_vsh_input_layout(mask, fvf, entry->vs_blob);
    entry->layouts[entry->layout_count]      = layout;
    entry->layout_masks[entry->layout_count] = key;
    entry->layout_count++;

    return layout;
}

/* ================================================================
 * Public API Implementation
 * ================================================================ */

HRESULT d3d8_vsh_init(void)
{
    D3D11_BUFFER_DESC cbd;
    HRESULT hr;

    memset(g_vsh_slots, 0, sizeof(g_vsh_slots));
    memset(g_vsh_cache, 0, sizeof(g_vsh_cache));
    memset(&g_vsh_constants, 0, sizeof(g_vsh_constants));
    g_vsh_slot_count = 0;
    g_vsh_constants_dirty = TRUE;

    /* Create the constant buffer for VS constants (192 * float4 = 3072 bytes) */
    memset(&cbd, 0, sizeof(cbd));
    cbd.ByteWidth      = sizeof(NV2AVSConstants);
    cbd.Usage           = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags       = D3D11_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags  = D3D11_CPU_ACCESS_WRITE;

    hr = ID3D11Device_CreateBuffer(d3d8_GetD3D11Device(), &cbd, NULL, &g_vsh_cb);
    if (FAILED(hr)) {
        fprintf(stderr, "D3D8 VSH: Failed to create constant buffer: 0x%08lX\n", hr);
        return hr;
    }

    fprintf(stderr, "D3D8 VSH: Vertex shader translator initialized\n");
    return S_OK;
}

void d3d8_vsh_shutdown(void)
{
    int i, j;

    /* Release all cached shaders and layouts */
    for (i = 0; i < NV2A_VS_CACHE_SIZE; i++) {
        VshCacheEntry *e = &g_vsh_cache[i];
        if (!e->in_use) continue;
        if (e->vs)      ID3D11VertexShader_Release(e->vs);
        if (e->vs_blob) ID3D10Blob_Release(e->vs_blob);
        for (j = 0; j < e->layout_count; j++) {
            if (e->layouts[j])
                ID3D11InputLayout_Release(e->layouts[j]);
        }
    }
    memset(g_vsh_cache, 0, sizeof(g_vsh_cache));

    if (g_vsh_cb) {
        ID3D11Buffer_Release(g_vsh_cb);
        g_vsh_cb = NULL;
    }

    memset(g_vsh_slots, 0, sizeof(g_vsh_slots));
    g_vsh_slot_count = 0;
}

HRESULT d3d8_vsh_create_shader(const DWORD *microcode, int num_insns,
                                DWORD *out_handle)
{
    int slot;

    if (!microcode || num_insns <= 0 || !out_handle)
        return E_INVALIDARG;

    if (num_insns > NV2A_VS_MAX_INSTRUCTIONS)
        num_insns = NV2A_VS_MAX_INSTRUCTIONS;

    /* Find a free slot */
    slot = -1;
    for (int i = 0; i < NV2A_VS_MAX_SLOTS; i++) {
        if (!g_vsh_slots[i].in_use) {
            slot = i;
            break;
        }
    }

    if (slot < 0) {
        fprintf(stderr, "D3D8 VSH: No free shader slots\n");
        return E_OUTOFMEMORY;
    }

    /* Store microcode (deferred compilation) */
    memcpy(g_vsh_slots[slot].microcode, microcode,
           (size_t)num_insns * 4 * sizeof(DWORD));
    g_vsh_slots[slot].length = num_insns;
    g_vsh_slots[slot].in_use = 1;
    g_vsh_slot_count++;

    /* Generate handle: slot index + 0x10000 to distinguish from FVF codes.
     * Xbox D3D8 uses handles with the high bit set (> 0xFFFF). */
    *out_handle = (DWORD)(slot + 0x10000);

    fprintf(stderr, "D3D8 VSH: Created shader handle 0x%lX (%d instructions)\n",
            *out_handle, num_insns);

    return S_OK;
}

HRESULT d3d8_vsh_delete_shader(DWORD handle)
{
    int slot;

    if (!d3d8_vsh_is_programmable(handle))
        return E_INVALIDARG;

    slot = (int)(handle - 0x10000);
    if (slot < 0 || slot >= NV2A_VS_MAX_SLOTS)
        return E_INVALIDARG;

    if (g_vsh_slots[slot].in_use) {
        g_vsh_slots[slot].in_use = 0;
        g_vsh_slot_count--;
    }

    return S_OK;
}

void d3d8_vsh_set_constant(int start_reg, const float *data, int count)
{
    int end_reg;

    if (!data || start_reg < 0)
        return;

    end_reg = start_reg + count;
    if (end_reg > NV2A_VS_MAX_CONSTANTS)
        end_reg = NV2A_VS_MAX_CONSTANTS;

    for (int i = start_reg; i < end_reg; i++) {
        int src_offset = (i - start_reg) * 4;
        g_vsh_constants.c[i][0] = data[src_offset + 0];
        g_vsh_constants.c[i][1] = data[src_offset + 1];
        g_vsh_constants.c[i][2] = data[src_offset + 2];
        g_vsh_constants.c[i][3] = data[src_offset + 3];
    }

    g_vsh_constants_dirty = TRUE;
}

BOOL d3d8_vsh_is_programmable(DWORD handle)
{
    return (handle >= 0x10000) ? TRUE : FALSE;
}

BOOL d3d8_vsh_prepare_draw(DWORD handle)
{
    ID3D11DeviceContext *ctx;
    int slot;
    NV2AVshSlot *vsh;
    uint32_t hash;
    VshCacheEntry *entry;
    ID3D11InputLayout *layout;
    D3D11_MAPPED_SUBRESOURCE mapped;
    HRESULT hr;

    if (!d3d8_vsh_is_programmable(handle))
        return FALSE;

    ctx = d3d8_GetD3D11Context();
    if (!ctx) return FALSE;

    /* Resolve handle to shader slot */
    slot = (int)(handle - 0x10000);
    if (slot < 0 || slot >= NV2A_VS_MAX_SLOTS)
        return FALSE;

    vsh = &g_vsh_slots[slot];
    if (!vsh->in_use)
        return FALSE;

    /* Hash the microcode to look up in cache */
    hash = fnv1a_hash(vsh->microcode, (size_t)vsh->length * 4 * sizeof(DWORD));

    /* Look up in cache */
    entry = cache_lookup(hash);
    if (!entry) {
        /* Cache miss: parse, generate HLSL, compile */
        entry = compile_shader(vsh->microcode, vsh->length, hash);
        if (!entry)
            return FALSE;
    }

    /* Bind the vertex shader */
    ID3D11DeviceContext_VSSetShader(ctx, entry->vs, NULL, 0);

    /* Bind the input layout (sizes texcoords from the bound stream FVF) */
    layout = get_cached_layout(entry, d3d8_GetCurrentFVF());
    if (layout)
        ID3D11DeviceContext_IASetInputLayout(ctx, layout);

    /* Update constant buffer if dirty */
    if (g_vsh_constants_dirty) {
        hr = ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)g_vsh_cb,
                                     0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        if (SUCCEEDED(hr)) {
            memcpy(mapped.pData, &g_vsh_constants, sizeof(g_vsh_constants));
            ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)g_vsh_cb, 0);
        }
        g_vsh_constants_dirty = FALSE;
    }

    /* Bind constant buffer to slot b1 */
    ID3D11DeviceContext_VSSetConstantBuffers(ctx, 1, 1, &g_vsh_cb);

    return TRUE;
}
