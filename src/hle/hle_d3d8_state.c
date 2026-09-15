/*
 * hle_d3d8_state.c -- the title's render and texture stage states, forwarded
 * to the shadow device before each draw.
 *
 * Part of shadow mode (hle_d3d8.c, RECOMP_HLE_D3D8=shadow).
 *
 * Xbox D3D8 inlines most SetRenderState and SetTextureStageState calls: the
 * title writes the value into D3D8's own state arrays and nothing is called
 * that a replacement could see. So, as Cxbx-Reloaded does (RenderStates.cpp,
 * TextureStates.cpp), the arrays themselves are read when a draw reaches the
 * host, and every state that changed since the last draw is converted and set.
 * The arrays are found by name through the title's XDK symbols:
 *   D3D_g_RenderState           render states, from X_D3DRS_PSALPHAINPUTS0;
 *   D3D_g_DeferredRenderState   the same array from X_D3DRS_FOGENABLE;
 *   D3D_g_ComplexRenderState    the same array from X_D3DRS_PSTEXTUREMODES;
 *   D3D_g_DeferredTextureState  4 stages x 32 texture stage states.
 *
 * Which slot holds which state changed until XDK 4627 (Cxbx-Reloaded,
 * DxbxRenderStateInfo in XbConvert.cpp). This file knows the final layout
 * only: deferred states start at 92 and complex states at 136, and the one
 * state removed along the way, D3DRS_MULTISAMPLETYPE (154), leaves every
 * later state one slot down. The distances between the named arrays say
 * whether a title has that layout; if not, nothing is forwarded and the run
 * log says why. Texture stage states use the order from XDK 4039 on.
 *
 * One gap in "the title writes the arrays": the XDK's own
 * D3DDevice_SetRenderState_Simple only writes the push buffer. Burnout 2
 * reaches it through SetRenderStateNotInline, which stores the value after;
 * a title that calls it directly would leave the simple states (57-91) stale
 * here, and Cxbx-Reloaded replaces that function to store them.
 *
 * Values are Xbox (OpenGL-style) enumerations; the host takes the PC's
 * (src/d3d/d3d8_xbox.h and the converters in d3d8_states.c). The tables
 * below are Cxbx-Reloaded's EmuXB2PC_* conversions (XbConvert.h). A value
 * with no host equivalent is replaced by the nearest one and logged once.
 *
 * Forwarded but not yet used by the host: SHADEMODE, DITHERENABLE,
 * COLORVERTEX, NORMALIZENORMALS, RESULTARG, BUMPENVMAT*, MIPMAPLODBIAS,
 * MAXMIPLEVEL, BORDERCOLOR. Not forwarded: LIGHTING (lights and material are
 * not forwarded, so it stays off), pixel shader states (0-56; pixel shaders
 * are not replayed yet), WRAP0-3, VERTEXBLEND, LOCALVIEWER, ZBIAS,
 * EDGEANTIALIAS, BLENDCOLOR, the back-face material and two-sided lighting
 * states, point sprite, material source and multisample states, and the
 * texture stage states ADDRESSW, TEXTURETRANSFORMFLAGS, BUMPENVLSCALE/LOFFSET,
 * COLORKEYOP, COLORSIGN, ALPHAKILL, COLORKEYCOLOR, COLORARG0 (its host number
 * is ALPHAKILL's) and ALPHAARG0.
 */
#include "platform/xbox_winnt.h"
#include <stdio.h>
#include <string.h>
#include "hle.h"

HLE_IMPORT_VAR(D3D_g_RenderState);
HLE_IMPORT_VAR(D3D_g_DeferredRenderState);
HLE_IMPORT_VAR(D3D_g_ComplexRenderState);
HLE_IMPORT_VAR(D3D_g_DeferredTextureState);

#ifdef _WIN32
#include "d3d8_xbox.h"

#define XRS_COUNT        167         /* X_D3DRS_DONOTCULLUNCOMPRESSED + 1 */
#define XRS_REMOVED      154         /* X_D3DRS_MULTISAMPLETYPE */
#define XRS_FOGENABLE    92
#define XRS_COMPLEX      136
#define STAGES           4
#define STAGE_SIZE       32

static int      g_state_ready = -1;  /* -1 not checked, 0 unusable, 1 ready */
static uint32_t g_prev_rs[XRS_COUNT];
static uint32_t g_prev_tss[STAGES][STAGE_SIZE];
static int      g_prev_valid;

/* Each value is logged once. Texture operations are converted on every draw,
 * so logging each occurrence would bury every other value under one. */
static void unmapped(const char *what, uint32_t value)
{
    static struct { const char *what; uint32_t value; } seen[32];
    static int nseen;
    int i;

    for (i = 0; i < nseen; i++)
        if (seen[i].value == value && strcmp(seen[i].what, what) == 0)
            return;
    if (nseen < (int)(sizeof seen / sizeof seen[0])) {
        seen[nseen].what = what;
        seen[nseen].value = value;
        nseen++;
        fprintf(stderr, "[HLE-D3D8] shadow state: %s value 0x%X has no host "
                "equivalent, nearest used\n", what, value);
    }
}

static int state_ready(void)
{
    uint32_t rs = hle_var_D3D_g_RenderState;

    if (g_state_ready >= 0)
        return g_state_ready;
    g_state_ready = 0;
    /* Both block starts are needed: XDKs before 4627 added the simple and
     * deferred unused slots separately, so the deferred start alone does not
     * pin where the complex states are. */
    if (!rs || !hle_var_D3D_g_DeferredRenderState || !hle_var_D3D_g_ComplexRenderState ||
        !hle_var_D3D_g_DeferredTextureState) {
        fprintf(stderr, "[HLE-D3D8] shadow states off: this title's XDK symbols do not "
                "name all of D3D_g_RenderState, D3D_g_DeferredRenderState, "
                "D3D_g_ComplexRenderState and D3D_g_DeferredTextureState\n");
        return 0;
    }
    if (hle_var_D3D_g_DeferredRenderState - rs != 4 * XRS_FOGENABLE ||
        hle_var_D3D_g_ComplexRenderState - rs != 4 * XRS_COMPLEX) {
        fprintf(stderr, "[HLE-D3D8] shadow states off: deferred states at slot %d and "
                "complex at %d, where the XDK 4627+ layout has %d and %d\n",
                (int)(hle_var_D3D_g_DeferredRenderState - rs) / 4,
                (int)(hle_var_D3D_g_ComplexRenderState - rs) / 4,
                XRS_FOGENABLE, XRS_COMPLEX);
        return 0;
    }
    fprintf(stderr, "[HLE-D3D8] shadow states: render states at 0x%08X, texture "
            "stage states at 0x%08X (XDK 4627+ layout)\n",
            rs, hle_var_D3D_g_DeferredTextureState);
    g_state_ready = 1;
    return 1;
}

static uint32_t guest_rs(uint32_t state)
{
    uint32_t slot = state > XRS_REMOVED ? state - 1 : state;
    return HLE_MEM32(hle_var_D3D_g_RenderState + 4 * slot);
}

/* ------------------------------------------------------------ conversions */

static DWORD cmp_to_host(uint32_t v)
{
    if (v >= 0x200 && v <= 0x207)
        return D3DCMP_NEVER + (v - 0x200);   /* NEVER..ALWAYS, same order */
    unmapped("D3DCMPFUNC", v);
    return D3DCMP_ALWAYS;
}

static DWORD blend_to_host(uint32_t v)
{
    if (v == 0)
        return D3DBLEND_ZERO;
    if (v == 1)
        return D3DBLEND_ONE;
    if (v >= 0x300 && v <= 0x308)
        return D3DBLEND_SRCCOLOR + (v - 0x300);  /* SRCCOLOR..SRCALPHASAT */
    unmapped("D3DBLEND", v);                 /* the constant-colour factors */
    return D3DBLEND_ONE;
}

static DWORD blendop_to_host(uint32_t v)
{
    switch (v) {                             /* d3d8_to_d3d11_blendop: 1..5 */
    case 0x8006: return 1;                   /* ADD */
    case 0x800A: return 2;                   /* SUBTRACT */
    case 0x800B: return 3;                   /* REVSUBTRACT */
    case 0x8007: return 4;                   /* MIN */
    case 0x8008: return 5;                   /* MAX */
    case 0xF006: unmapped("D3DBLENDOP", v); return 1;   /* ADDSIGNED */
    case 0xF005: unmapped("D3DBLENDOP", v); return 3;   /* REVSUBTRACTSIGNED */
    }
    unmapped("D3DBLENDOP", v);
    return 1;
}

static DWORD stencilop_to_host(uint32_t v)
{
    switch (v) {                             /* d3d8_to_d3d11_stencilop: 1..8 */
    case 0x1E00: return 1;                   /* KEEP */
    case 0x0000: return 2;                   /* ZERO */
    case 0x1E01: return 3;                   /* REPLACE */
    case 0x1E02: return 4;                   /* INCRSAT */
    case 0x1E03: return 5;                   /* DECRSAT */
    case 0x150A: return 6;                   /* INVERT */
    case 0x8507: return 7;                   /* INCR */
    case 0x8508: return 8;                   /* DECR */
    }
    unmapped("D3DSTENCILOP", v);
    return 1;
}

static DWORD colorwrite_to_host(uint32_t v)
{
    /* Xbox keeps a byte per channel, A R G B from the top; the host takes
     * the PC's RED 1, GREEN 2, BLUE 4, ALPHA 8. */
    return ((v & 0x00010000) ? 1u : 0u) | ((v & 0x00000100) ? 2u : 0u) |
           ((v & 0x00000001) ? 4u : 0u) | ((v & 0x01000000) ? 8u : 0u);
}

static DWORD cull_to_host(uint32_t v)
{
    switch (v) {
    case 0:     return D3DCULL_NONE;
    case 0x900: return D3DCULL_CW;
    case 0x901: return D3DCULL_CCW;
    }
    unmapped("D3DCULL", v);
    return D3DCULL_NONE;
}

static DWORD fill_to_host(uint32_t v)
{
    if (v >= 0x1B00 && v <= 0x1B02)
        return D3DFILL_POINT + (v - 0x1B00);
    unmapped("D3DFILLMODE", v);
    return D3DFILL_SOLID;
}

static DWORD shade_to_host(uint32_t v)
{
    if (v == 0x1D00)
        return 1;                            /* D3DSHADE_FLAT */
    if (v == 0x1D01)
        return 2;                            /* D3DSHADE_GOURAUD */
    unmapped("D3DSHADEMODE", v);
    return 2;
}

/* Xbox D3DTOP (Cxbx-Reloaded, XbD3D8Types.h, XDK 4039 on) to the host's own
 * enumeration, which numbers the blend ops differently and lacks some. */
static DWORD textureop_to_host(uint32_t v)
{
    switch (v) {
    case 1:  return D3DTOP_DISABLE;
    case 2:  return D3DTOP_SELECTARG1;
    case 3:  return D3DTOP_SELECTARG2;
    case 4:  return D3DTOP_MODULATE;
    case 5:  return D3DTOP_MODULATE2X;
    case 6:  return D3DTOP_MODULATE4X;
    case 7:  return D3DTOP_ADD;
    case 8:  return D3DTOP_ADDSIGNED;
    case 9:  return D3DTOP_ADDSIGNED2X;
    case 10: return D3DTOP_SUBTRACT;
    case 11: return D3DTOP_ADDSMOOTH;
    case 12: return D3DTOP_BLENDDIFFUSEALPHA;
    case 13: return D3DTOP_BLENDCURRENTALPHA;
    case 14: return D3DTOP_BLENDTEXTUREALPHA;
    case 15: return D3DTOP_BLENDFACTORALPHA;
    case 16: unmapped("D3DTOP BLENDTEXTUREALPHAPM", v); return D3DTOP_BLENDTEXTUREALPHA;
    case 17: return D3DTOP_PREMODULATE;
    case 22: return D3DTOP_DOTPRODUCT3;
    case 23: return D3DTOP_MULTIPLYADD;
    case 24: return D3DTOP_LERP;
    }
    unmapped("D3DTOP", v);                   /* 18-21 and the bump-map ops */
    return D3DTOP_MODULATE;
}

static DWORD address_to_host(uint32_t v)
{
    if (v >= 1 && v <= 4)                    /* WRAP, MIRROR, CLAMP, BORDER */
        return v;
    if (v == 5)                              /* CLAMPTOEDGE; host 5 is MIRRORONCE */
        return D3DTADDRESS_CLAMP;
    if (v)
        unmapped("D3DTEXTUREADDRESS", v);
    return D3DTADDRESS_WRAP;
}

static DWORD filter_to_host(uint32_t v)
{
    if (v <= 3)                              /* NONE, POINT, LINEAR, ANISOTROPIC */
        return v;
    unmapped("D3DTEXTUREFILTER", v);         /* QUINCUNX, GAUSSIANCUBIC */
    return D3DTEXF_LINEAR;
}

/* ------------------------------------------------------------------ apply */

typedef enum {
    AS_IS, CMP, BLEND, BLENDOP, STENCILOP, COLORWRITE, CULL, FILL, SHADE
} rs_kind;

static const struct {
    uint16_t xbox;
    uint16_t host;
    uint8_t  kind;
} g_rs_map[] = {
    {  57, D3DRS_ZFUNC,            CMP },
    {  58, D3DRS_ALPHAFUNC,        CMP },
    {  59, D3DRS_ALPHABLENDENABLE, AS_IS },
    {  60, D3DRS_ALPHATESTENABLE,  AS_IS },
    {  61, D3DRS_ALPHAREF,         AS_IS },
    {  62, D3DRS_SRCBLEND,         BLEND },
    {  63, D3DRS_DESTBLEND,        BLEND },
    {  64, D3DRS_ZWRITEENABLE,     AS_IS },
    {  65, D3DRS_DITHERENABLE,     AS_IS },
    {  66, D3DRS_SHADEMODE,        SHADE },
    {  67, D3DRS_COLORWRITEENABLE, COLORWRITE },
    {  68, D3DRS_STENCILZFAIL,     STENCILOP },
    {  69, D3DRS_STENCILPASS,      STENCILOP },
    {  70, D3DRS_STENCILFUNC,      CMP },
    {  71, D3DRS_STENCILREF,       AS_IS },
    {  72, D3DRS_STENCILMASK,      AS_IS },
    {  73, D3DRS_STENCILWRITEMASK, AS_IS },
    {  74, D3DRS_BLENDOP,          BLENDOP },
    {  92, D3DRS_FOGENABLE,        AS_IS },
    {  93, D3DRS_FOGTABLEMODE,     AS_IS },
    {  94, D3DRS_FOGSTART,         AS_IS },  /* float bits */
    {  95, D3DRS_FOGEND,           AS_IS },
    {  96, D3DRS_FOGDENSITY,       AS_IS },
    {  97, D3DRS_RANGEFOGENABLE,   AS_IS },
    /* D3DRS_LIGHTING (102) is held off: lights and the material are not
     * forwarded, and the host would light every vertex with none -- black. */
    { 103, D3DRS_SPECULARENABLE,   AS_IS },
    { 105, D3DRS_COLORVERTEX,      AS_IS },
    { 115, D3DRS_AMBIENT,          AS_IS },
    { 138, D3DRS_FOGCOLOR,         AS_IS },
    { 139, D3DRS_FILLMODE,         FILL },
    { 142, D3DRS_NORMALIZENORMALS, AS_IS },
    { 143, D3DRS_ZENABLE,          AS_IS },
    { 144, D3DRS_STENCILENABLE,    AS_IS },
    { 145, D3DRS_STENCILFAIL,      STENCILOP },
    { 147, D3DRS_CULLMODE,         CULL },
    { 148, D3DRS_TEXTUREFACTOR,    AS_IS },
};

static DWORD rs_to_host(uint8_t kind, uint32_t v)
{
    switch (kind) {
    case CMP:        return cmp_to_host(v);
    case BLEND:      return blend_to_host(v);
    case BLENDOP:    return blendop_to_host(v);
    case STENCILOP:  return stencilop_to_host(v);
    case COLORWRITE: return colorwrite_to_host(v);
    case CULL:       return cull_to_host(v);
    case FILL:       return fill_to_host(v);
    case SHADE:      return shade_to_host(v);
    }
    return v;
}

/* Xbox texture stage state slot (XDK 4039 order) to the host's. */
typedef enum { TS_AS_IS, TS_OP, TS_ADDRESS, TS_FILTER, TS_TEXCOORD } ts_kind;

static const struct {
    uint8_t xbox;
    uint8_t host;
    uint8_t kind;
} g_ts_map[] = {
    {  0, D3DTSS_ADDRESSU,      TS_ADDRESS },
    {  1, D3DTSS_ADDRESSV,      TS_ADDRESS },
    {  3, D3DTSS_MAGFILTER,     TS_FILTER },
    {  4, D3DTSS_MINFILTER,     TS_FILTER },
    {  5, D3DTSS_MIPFILTER,     TS_FILTER },
    {  6, D3DTSS_MIPMAPLODBIAS, TS_AS_IS },
    {  7, D3DTSS_MAXMIPLEVEL,   TS_AS_IS },
    {  8, D3DTSS_MAXANISOTROPY, TS_AS_IS },
    { 12, D3DTSS_COLOROP,       TS_OP },
    { 14, D3DTSS_COLORARG1,     TS_AS_IS },  /* D3DTA_* match the PC's */
    { 15, D3DTSS_COLORARG2,     TS_AS_IS },
    { 16, D3DTSS_ALPHAOP,       TS_OP },
    { 18, D3DTSS_ALPHAARG1,     TS_AS_IS },
    { 19, D3DTSS_ALPHAARG2,     TS_AS_IS },
    { 20, D3DTSS_RESULTARG,     TS_AS_IS },
    { 22, D3DTSS_BUMPENVMAT00,  TS_AS_IS },
    { 23, D3DTSS_BUMPENVMAT01,  TS_AS_IS },
    { 24, D3DTSS_BUMPENVMAT11,  TS_AS_IS },  /* Xbox has 11 before 10 */
    { 25, D3DTSS_BUMPENVMAT10,  TS_AS_IS },
    { 28, D3DTSS_TEXCOORDINDEX, TS_TEXCOORD },
    { 29, D3DTSS_BORDERCOLOR,   TS_AS_IS },
};

static DWORD ts_to_host(uint8_t kind, uint32_t v)
{
    switch (kind) {
    case TS_OP:       return textureop_to_host(v);
    case TS_ADDRESS:  return address_to_host(v);
    case TS_FILTER:   return filter_to_host(v);
    case TS_TEXCOORD: {
        /* Index in the low word, generation mode in the high word. Modes up to
         * 0x30000 are the PC's. Xbox sphere mapping is 0x50000 where the host
         * has 0x40000, and Xbox object-linear (0x40000) has no host value
         * (Cxbx-Reloaded, TextureStates.cpp). */
        uint32_t index = v & 3, mode = v & 0xFFFF0000;

        if ((v & 0xFFFF) > 3)
            unmapped("D3DTSS_TEXCOORDINDEX index", v);
        if (mode <= 0x00030000)
            return mode | index;
        if (mode == 0x00050000)
            return D3DTSS_TCI_SPHEREMAP | index;
        unmapped("D3DTSS_TCI", v);
        return index;
    }
    }
    return v;
}

void hle_d3d8_shadow_apply_states(IDirect3DDevice8 *dev)
{
    const IDirect3DDevice8Vtbl *vt;
    size_t i;
    int s;

    if (!dev || !state_ready())
        return;
    vt = dev->lpVtbl;

    for (i = 0; i < sizeof g_rs_map / sizeof g_rs_map[0]; i++) {
        uint32_t x = g_rs_map[i].xbox;
        uint32_t v = guest_rs(x);

        if (g_prev_valid && g_prev_rs[x] == v)
            continue;
        g_prev_rs[x] = v;
        vt->SetRenderState(dev, (D3DRENDERSTATETYPE)g_rs_map[i].host,
                           rs_to_host(g_rs_map[i].kind, v));
    }

    for (s = 0; s < STAGES; s++) {
        uint32_t base = hle_var_D3D_g_DeferredTextureState + (uint32_t)(s * STAGE_SIZE * 4);

        for (i = 0; i < sizeof g_ts_map / sizeof g_ts_map[0]; i++) {
            uint32_t x = g_ts_map[i].xbox;
            uint32_t v = HLE_MEM32(base + 4 * x);

            /* The host's SetTexture rewrites COLOROP whenever a texture is
             * bound or unbound, so the operations are set on every draw. */
            if (g_prev_valid && g_prev_tss[s][x] == v && g_ts_map[i].kind != TS_OP)
                continue;
            g_prev_tss[s][x] = v;
            vt->SetTextureStageState(dev, (DWORD)s,
                                     (D3DTEXTURESTAGESTATETYPE)g_ts_map[i].host,
                                     ts_to_host(g_ts_map[i].kind, v));
        }
    }
    g_prev_valid = 1;
}
#endif /* _WIN32 */
