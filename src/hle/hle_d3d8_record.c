/*
 * hle_d3d8_record.c -- shadow mode's host boundary, and frame capture at it.
 *
 * Part of shadow mode (hle_d3d8.c, RECOMP_HLE_D3D8=shadow). What the
 * wrappers are for, and the rule that every host call goes through them, is
 * in hle_d3d8_record.h; the capture format is in d3d8_capture.h.
 *
 * The default capture swap is 120 because the first frames of a title are
 * its loader: nothing is bound, most state is still at the device's defaults,
 * and the capture would be of an empty screen. 120 swaps is far enough in to
 * have textures and a real draw list, and still inside the first minute.
 *
 * RECOMP_D3D8_CAPTURE_EVERY=<n> (n >= 2) keeps capturing: after the frame
 * at the requested swap, one more every n swaps, each to <path>_<swap> with
 * the capture extension, at most CAPTURE_MAX_FILES files. A title never
 * reaches the same moment at the same swap twice, so one long run captured
 * throughout is how to get the frame wanted; the files are picked afterwards.
 *
 * Frame boundaries: recording starts when the swap counter reaches the
 * requested swap and stops at the next one, so the file holds the host calls
 * between one Swap and the next. Most of the state those calls draw with was
 * set in earlier frames, so the capture opens with a snapshot read back from
 * the host renderer itself (capture_snapshot). Reading the host rather than
 * asking src/hle to re-emit what it holds is what makes the capture
 * independent of src/hle's change caches: whatever they believe, the snapshot
 * is what the device actually had.
 *
 * Not handled:
 *   - a stage bound to a cube or volume texture is recorded as nothing bound,
 *     and counted. src/hle creates neither, so this does not happen today.
 *   - more than CAPTURE_MAX_TEXTURES distinct textures in one frame: the rest
 *     are recorded as nothing bound, and counted.
 *   - calls made while the capture file is being closed on another thread;
 *     see "Threads" in hle_d3d8_record.h.
 */
#include "platform/xbox_winnt.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32

#include "d3d8_xbox.h"
#include "d3d8_internal.h"
#include "d3d8_vsh.h"
#include "d3d8_combiners.h"
#include "d3d8_capture.h"
#include "hle_d3d8_record.h"

/* hle_d3d8.c: the shadow device, or NULL. */
IDirect3DDevice8 *hle_d3d8_shadow_device(void);

#define CAPTURE_DEFAULT_SWAP    120
#define CAPTURE_MAX_TEXTURES    1024
#define CAPTURE_MAX_FILES       24   /* as RECOMP_HLE_D3D8_DUMP */
#define CAPTURE_MAX_LEVELS      16
#define CAPTURE_STAGES          4    /* d3d8_device.c MAX_TEXTURE_STAGES */
#define CAPTURE_RENDER_STATES   256  /* d3d8_device.c MAX_RENDER_STATES */
#define CAPTURE_STAGE_STATES    32   /* d3d8_device.c MAX_TSS_STATES */

static D3D8CapWriter *g_cap;
static int            g_configured;
static const char    *g_path;
static unsigned long  g_target_swap;
static unsigned long  g_frame;
static unsigned long  g_every;       /* RECOMP_D3D8_CAPTURE_EVERY, 0 = once */
static unsigned long  g_min_draws;   /* RECOMP_D3D8_CAPTURE_MINDRAWS, 0 = any */
static unsigned       g_files;
static char           g_file[1024];  /* the file being written */

/* Host texture objects whose contents are in this capture, and their ids. */
static struct {
    IDirect3DBaseTexture8 *object;
    uint32_t               id;
} g_textures[CAPTURE_MAX_TEXTURES];
static int      g_texture_count;
static uint32_t g_next_texture_id;

/* Host depth surfaces a SET_RENDER_TARGET in this capture has named. Shadow
 * mode keeps its depth surfaces for the life of the process (hle_d3d8.c,
 * depth_surface), so a pointer never comes back as a different surface. */
#define CAPTURE_MAX_DEPTHS 16
static IDirect3DSurface8 *g_depth_objects[CAPTURE_MAX_DEPTHS];
static int                g_depth_count;
static IDirect3DSurface8 *g_device_depth;   /* host_DeviceDepthSurface */

/* The render target the wrappers last set, for the snapshot. Kept here
 * because the host's surface for a texture level does not lead back to the
 * texture; host_SetRenderTarget is the only place the target changes, so
 * this is the host's own. g_target_lost: the snapshot cannot name the
 * target -- the bound texture was released (the host still draws into it
 * through its surface), or the host refused the last switch. */
static IDirect3DBaseTexture8 *g_target_texture;
static UINT               g_target_level, g_target_face;
static IDirect3DSurface8 *g_target_depth;
static int                g_target_lost;

static unsigned long g_draws, g_texture_writes, g_level_writes, g_unrecorded_binds;
static unsigned long g_unrecorded_targets;
static uint64_t      g_texture_bytes;

int hle_d3d8_capture_active(void)
{
    return g_cap != NULL;
}

static void chunk(uint32_t type, const void *p0, size_t n0, const void *p1, size_t n1,
                  const void *p2, size_t n2)
{
    d3d8cap_chunk(g_cap, type, p0, n0, p1, n1, p2, n2);
}

/* ---------------------------------------------------------------- textures */

static int texture_find(IDirect3DBaseTexture8 *object)
{
    int i;

    for (i = 0; i < g_texture_count; i++)
        if (g_textures[i].object == object)
            return i;
    return -1;
}

/* A cube texture's creation, with no contents. d3d8_cube_info is also the
 * type test: it answers only for a cube. */
static int cube_write(IDirect3DBaseTexture8 *object, uint32_t id)
{
    D3D8CubeInfo info;
    D3D8CapCubeTexture head;

    if (!d3d8_cube_info(object, &info))
        return 0;
    head.id     = id;
    head.format = (uint32_t)info.format;
    head.edge   = info.edge;
    head.levels = info.levels;
    head.usage  = info.usage;
    chunk(D3D8CAP_CUBE_TEXTURE, &head, sizeof head, NULL, 0, NULL, 0);
    g_texture_writes++;
    return 1;
}

/* The whole texture as the host holds it. The host keeps its levels back to
 * back in one allocation (d3d8_internal.h, D3D8Texture.sys_mem), so they go
 * out as one run of bytes; that layout is checked rather than assumed. */
static int texture_write(IDirect3DBaseTexture8 *object, uint32_t id)
{
    D3D8TextureInfo info;
    D3D8CapTexture head;
    D3D8CapLevel levels[CAPTURE_MAX_LEVELS];
    const BYTE *first = NULL;
    size_t total = 0;
    UINT l;

    if (!d3d8_texture_info(object, &info) || !info.levels ||
        info.levels > CAPTURE_MAX_LEVELS)
        return 0;
    for (l = 0; l < info.levels; l++) {
        const BYTE *bits;
        UINT pitch, rows;

        if (!d3d8_texture_level(object, l, &bits, &pitch, &rows))
            return 0;
        if (!l)
            first = bits;
        else if (bits != first + total)
            return 0;
        levels[l].pitch = pitch;
        levels[l].rows  = rows;
        levels[l].bytes = pitch * rows;
        total += levels[l].bytes;
    }
    head.id     = id;
    head.format = (uint32_t)info.format;
    head.width  = info.width;
    head.height = info.height;
    head.levels = info.levels;
    head.usage  = info.usage;
    chunk(D3D8CAP_TEXTURE, &head, sizeof head,
          levels, (size_t)info.levels * sizeof levels[0], first, total);
    g_texture_writes++;
    g_texture_bytes += total;
    return 1;
}

/* The capture id for a texture about to be bound, writing it first if the
 * capture does not hold it yet. 0 for NULL, and for a texture that cannot be
 * recorded (counted). */
static uint32_t texture_id(IDirect3DBaseTexture8 *object)
{
    int i;

    if (!object)
        return 0;
    i = texture_find(object);
    if (i >= 0)
        return g_textures[i].id;
    if (g_texture_count >= CAPTURE_MAX_TEXTURES ||
        (!texture_write(object, g_next_texture_id) &&
         !cube_write(object, g_next_texture_id))) {
        g_unrecorded_binds++;
        return 0;
    }
    g_textures[g_texture_count].object = object;
    g_textures[g_texture_count].id     = g_next_texture_id;
    g_texture_count++;
    return g_next_texture_id++;
}

/* ------------------------------------------------ record, shared with the
 * snapshot so a state reaches the file the same way whichever wrote it */

static void rec_render_state(DWORD state, DWORD value)
{
    D3D8CapRenderState c;

    c.state = state;
    c.value = value;
    chunk(D3D8CAP_RENDER_STATE, &c, sizeof c, NULL, 0, NULL, 0);
}

static void rec_stage_state(DWORD stage, DWORD type, DWORD value)
{
    D3D8CapStageState c;

    c.stage = stage;
    c.type  = type;
    c.value = value;
    chunk(D3D8CAP_TEXTURE_STAGE_STATE, &c, sizeof c, NULL, 0, NULL, 0);
}

static void rec_transform(DWORD state, const D3DMATRIX *m)
{
    D3D8CapTransform c;

    c.state = state;
    memcpy(c.m, m, sizeof c.m);
    chunk(D3D8CAP_TRANSFORM, &c, sizeof c, NULL, 0, NULL, 0);
}

static void rec_viewport(const D3DVIEWPORT8 *vp)
{
    D3D8CapViewport c;

    c.x      = vp->X;
    c.y      = vp->Y;
    c.width  = vp->Width;
    c.height = vp->Height;
    c.min_z  = vp->MinZ;
    c.max_z  = vp->MaxZ;
    chunk(D3D8CAP_VIEWPORT, &c, sizeof c, NULL, 0, NULL, 0);
}

static void rec_set_texture(DWORD stage, IDirect3DBaseTexture8 *texture)
{
    D3D8CapSetTexture c;

    c.stage      = stage;
    c.texture_id = texture_id(texture);
    chunk(D3D8CAP_SET_TEXTURE, &c, sizeof c, NULL, 0, NULL, 0);
}

static void rec_set_vertex_shader(DWORD handle)
{
    D3D8CapSetVertexShader c;

    c.handle = handle;
    chunk(D3D8CAP_SET_VERTEX_SHADER, &c, sizeof c, NULL, 0, NULL, 0);
}

static void rec_vs_create(DWORD handle, const DWORD *microcode, int insn_count)
{
    D3D8CapVsCreate c;

    c.handle     = handle;
    c.insn_count = (uint32_t)insn_count;
    /* DWORD is 32 bits on Windows, so the microcode goes out as it is. */
    chunk(D3D8CAP_VS_CREATE, &c, sizeof c,
          microcode, (size_t)insn_count * 4u * sizeof(DWORD), NULL, 0);
}

static void rec_vs_declaration(DWORD handle, const D3D8VshInput *inputs, int count)
{
    D3D8CapVsDeclaration c;
    D3D8CapVsInput out[NV2A_VS_MAX_INPUTS];
    int i;

    if (count < 0 || count > NV2A_VS_MAX_INPUTS || (count && !inputs))
        return;                          /* the host refuses it too */
    c.handle = handle;
    c.count  = (uint32_t)count;
    for (i = 0; i < count; i++) {
        out[i].reg         = inputs[i].reg;
        out[i].dxgi_format = (uint32_t)inputs[i].format;
        out[i].offset      = inputs[i].offset;
    }
    chunk(D3D8CAP_VS_DECLARATION, &c, sizeof c,
          out, (size_t)count * sizeof out[0], NULL, 0);
}

static void rec_vs_constants(int first_reg, const float *data, int count)
{
    D3D8CapVsConstants c;

    /* The host clamps to its 192 registers (d3d8_vsh_set_constant); the
     * capture keeps what it actually read. */
    if (!data || first_reg < 0 || first_reg >= NV2A_VS_MAX_CONSTANTS || count <= 0)
        return;
    if (count > NV2A_VS_MAX_CONSTANTS - first_reg)
        count = NV2A_VS_MAX_CONSTANTS - first_reg;
    c.first_reg = (uint32_t)first_reg;
    c.count     = (uint32_t)count;
    chunk(D3D8CAP_VS_CONSTANTS, &c, sizeof c,
          data, (size_t)count * 4u * sizeof(float), NULL, 0);
}

static void rec_vs_screenspace(int enabled, const float scale[4], const float offset[4])
{
    D3D8CapVsScreenspace c;

    c.enabled = enabled != 0;
    memcpy(c.scale, scale, sizeof c.scale);
    memcpy(c.offset, offset, sizeof c.offset);
    chunk(D3D8CAP_VS_SCREENSPACE, &c, sizeof c, NULL, 0, NULL, 0);
}

static void rec_vs_vertex_data(int reg, const float value[4])
{
    D3D8CapVsVertexData c;

    if (reg < 0 || reg >= NV2A_VS_MAX_INPUTS)
        return;                          /* the host ignores it too */
    c.reg = (uint32_t)reg;
    memcpy(c.value, value, sizeof c.value);
    chunk(D3D8CAP_VS_VERTEX_DATA, &c, sizeof c, NULL, 0, NULL, 0);
}

static void rec_ps_token(DWORD token)
{
    D3D8CapPsToken c;

    c.token = token;
    chunk(D3D8CAP_PS_TOKEN, &c, sizeof c, NULL, 0, NULL, 0);
}

/* The capture id for a depth surface, writing it first if needed. 0 for
 * NULL, and for one that cannot be recorded (the caller counts it). */
static uint32_t depth_id(IDirect3DSurface8 *object)
{
    D3DSURFACE_DESC desc;
    D3D8CapDepthSurface c;
    int i;

    if (!object)
        return 0;
    if (object == g_device_depth)
        return D3D8CAP_DEPTH_DEVICE;
    for (i = 0; i < g_depth_count; i++)
        if (g_depth_objects[i] == object)
            return (uint32_t)i + 1;
    if (g_depth_count >= CAPTURE_MAX_DEPTHS ||
        FAILED(object->lpVtbl->GetDesc(object, &desc)))
        return 0;
    g_depth_objects[g_depth_count++] = object;
    c.id     = (uint32_t)g_depth_count;
    c.width  = desc.Width;
    c.height = desc.Height;
    c.format = (uint32_t)desc.Format;
    chunk(D3D8CAP_DEPTH_SURFACE, &c, sizeof c, NULL, 0, NULL, 0);
    return c.id;
}

/* A target the capture cannot name is recorded as the back buffer, which is
 * where replay would draw without it anyway, and counted. */
static void rec_set_render_target(IDirect3DBaseTexture8 *texture, UINT level,
                                  UINT face, IDirect3DSurface8 *depth, int lost)
{
    D3D8CapSetRenderTarget c;
    unsigned long binds = g_unrecorded_binds;

    /* texture_id counts a failure as a texture bind; this one is counted
     * below, as a target. */
    c.texture_id = texture_id(texture);
    g_unrecorded_binds = binds;
    c.level      = c.texture_id ? level : 0;
    c.face       = c.texture_id ? face : 0;
    c.depth_id   = depth_id(depth);
    if (lost || (texture && !c.texture_id) || (depth && !c.depth_id))
        g_unrecorded_targets++;
    chunk(D3D8CAP_SET_RENDER_TARGET, &c, sizeof c, NULL, 0, NULL, 0);
}

/* ---------------------------------------------------------------- snapshot */

/* Everything the frame draws with that was set before it began, read from the
 * host. The order is the one d3d8_capture.h documents: SetTexture rewrites
 * COLOROP, so the stage states come after the textures. */
static void capture_snapshot(IDirect3DDevice8 *dev)
{
    static const DWORD transforms[] = {
        D3DTS_VIEW, D3DTS_PROJECTION,
        D3DTS_TEXTURE0, D3DTS_TEXTURE0 + 1, D3DTS_TEXTURE0 + 2, D3DTS_TEXTURE0 + 3,
        D3DTS_WORLD, D3DTS_WORLD + 1, D3DTS_WORLD + 2, D3DTS_WORLD + 3
    };
    const DWORD *rs = d3d8_GetRenderStates();
    const float *constants = d3d8_vsh_constants();
    float scale[4], offset[4];
    int enabled, slot, i;
    DWORD vs = 0, s, t;
    D3DVIEWPORT8 vp;

    for (slot = 0; slot < NV2A_VS_MAX_SLOTS; slot++) {
        const DWORD *microcode;
        const D3D8VshInput *decl;
        int length, decl_count;
        DWORD handle;

        if (!d3d8_vsh_get_slot(slot, &handle, &microcode, &length, &decl, &decl_count))
            continue;
        rec_vs_create(handle, microcode, length);
        if (decl_count)
            rec_vs_declaration(handle, decl, decl_count);
    }
    if (constants)
        rec_vs_constants(0, constants, NV2A_VS_MAX_CONSTANTS);
    d3d8_vsh_get_screenspace(scale, offset, &enabled);
    rec_vs_screenspace(enabled, scale, offset);
    for (i = 0; i < NV2A_VS_MAX_INPUTS; i++) {
        float value[4];

        d3d8_vsh_get_vertex_data(i, value);
        rec_vs_vertex_data(i, value);
    }
    rec_ps_token(d3d8_combiners_get_pixel_shader());

    dev->lpVtbl->GetVertexShader(dev, &vs);
    rec_set_vertex_shader(vs);
    for (s = 0; s < CAPTURE_STAGES; s++)
        rec_set_texture(s, d3d8_GetStageTexture(s));
    rec_set_render_target(g_target_texture, g_target_level, g_target_face,
                          g_target_depth, g_target_lost);

    for (i = 0; i < (int)(sizeof transforms / sizeof transforms[0]); i++) {
        const D3DMATRIX *m = d3d8_GetTransform((D3DTRANSFORMSTATETYPE)transforms[i]);
        if (m)
            rec_transform(transforms[i], m);
    }
    dev->lpVtbl->GetViewport(dev, &vp);
    rec_viewport(&vp);

    for (s = 0; rs && s < CAPTURE_RENDER_STATES; s++)
        rec_render_state(s, rs[s]);
    for (s = 0; s < CAPTURE_STAGES; s++) {
        const DWORD *tss = d3d8_GetTSS(s);
        for (t = 0; tss && t < CAPTURE_STAGE_STATES; t++)
            rec_stage_state(s, t, tss[t]);
    }

    chunk(D3D8CAP_FRAME_START, NULL, 0, NULL, 0, NULL, 0);
}

/* ------------------------------------------------------------ frame boundary */

static void capture_configure(void)
{
    const char *swap, *every, *min_draws;

    g_configured = 1;
    g_path = getenv("RECOMP_D3D8_CAPTURE");
    g_target_swap = CAPTURE_DEFAULT_SWAP;
    swap = getenv("RECOMP_D3D8_CAPTURE_SWAP");
    if (swap && atol(swap) > 0)
        g_target_swap = (unsigned long)atol(swap);
    every = getenv("RECOMP_D3D8_CAPTURE_EVERY");
    if (every && atol(every) >= 2)
        g_every = (unsigned long)atol(every);
    min_draws = getenv("RECOMP_D3D8_CAPTURE_MINDRAWS");
    if (min_draws && atol(min_draws) > 0)
        g_min_draws = (unsigned long)atol(min_draws);
    if (g_path && *g_path)
        fprintf(stderr, "[HLE-D3D8] capture armed: swap %lu%s%s -> %s (host calls, "
                "format v%u)\n", g_target_swap,
                g_min_draws ? " or the first frame after it with enough draws" : "",
                g_every ? ", then every RECOMP_D3D8_CAPTURE_EVERY swaps" : "",
                g_path, D3D8CAP_VERSION);
}

void hle_d3d8_capture_swap(unsigned long swaps, uint32_t width, uint32_t height)
{
    IDirect3DDevice8 *dev;

    if (!g_configured)
        capture_configure();
    if (!g_path || !*g_path)
        return;

    if (g_cap) {
        D3D8CapWriter *w = g_cap;
        uint32_t chunks = d3d8cap_chunk_count(w);
        int ok;

        g_cap = NULL;                    /* stop recording before closing */
        ok = d3d8cap_close(w) == 0;
        if (g_min_draws && g_draws < g_min_draws) {
            /* Too few draws: dropped, and this swap starts the next try. */
            remove(g_file);
            g_unrecorded_targets = g_unrecorded_binds = 0;
            g_target_swap = swaps;
            goto start;
        }
        fprintf(stderr, "[HLE-D3D8] capture: frame %lu written to %s -- %u chunks, "
                "%lu draws, %lu textures (%llu bytes), %lu level refills%s\n",
                g_frame, g_file, chunks, g_draws, g_texture_writes,
                (unsigned long long)g_texture_bytes, g_level_writes,
                ok ? "" : " (INCOMPLETE: write failed)");
        if (g_unrecorded_targets)
            fprintf(stderr, "[HLE-D3D8] capture: %lu render target switches recorded "
                    "as the back buffer (target released or refused, or table full)\n",
                    g_unrecorded_targets);
        if (g_unrecorded_binds)
            fprintf(stderr, "[HLE-D3D8] capture: %lu texture binds recorded as "
                    "nothing bound (not a 2D texture, or table full)\n",
                    g_unrecorded_binds);
        fflush(stderr);
        g_files++;
        if (g_every && g_files < CAPTURE_MAX_FILES)
            g_target_swap = g_frame + g_every;
        else
            g_path = NULL;               /* done: one frame, or the file cap */
        return;
    }
start:
    if (swaps != g_target_swap)
        return;
    dev = hle_d3d8_shadow_device();
    if (!dev)
        return;

    g_frame = swaps;
    g_texture_count = 0;
    g_next_texture_id = 1;
    g_depth_count = 0;
    g_draws = g_texture_writes = g_level_writes = g_unrecorded_binds = 0;
    g_unrecorded_targets = 0;
    g_texture_bytes = 0;
    if (g_every) {
        /* <path>_<swap><extension>, with the extension moved to the end. */
        size_t n = strlen(g_path), e = strlen(D3D8CAP_EXTENSION);

        if (n >= e && strcmp(g_path + n - e, D3D8CAP_EXTENSION) == 0)
            n -= e;
        snprintf(g_file, sizeof g_file, "%.*s_%05lu%s", (int)n, g_path, swaps,
                 D3D8CAP_EXTENSION);
    } else {
        snprintf(g_file, sizeof g_file, "%s", g_path);
    }
    g_cap = d3d8cap_create(g_file, (uint32_t)swaps, width, height);
    if (!g_cap) {
        fprintf(stderr, "[HLE-D3D8] capture: cannot write %s; capture off\n", g_file);
        g_path = NULL;                   /* one attempt, not one per swap */
        return;
    }
    capture_snapshot(dev);
}

/* ------------------------------------------------------------ device calls */

HRESULT host_Clear(IDirect3DDevice8 *dev, DWORD count, const D3DRECT *rects,
                   DWORD flags, D3DCOLOR color, float z, DWORD stencil)
{
    if (g_cap) {
        D3D8CapClear c;
        D3D8CapRect r[16];
        DWORD i, n = rects ? count : 0;

        if (n > 16)                      /* src/hle passes none; more is a bug */
            n = 16;
        for (i = 0; i < n; i++) {
            r[i].x1 = rects[i].x1;
            r[i].y1 = rects[i].y1;
            r[i].x2 = rects[i].x2;
            r[i].y2 = rects[i].y2;
        }
        c.rect_count = n;
        c.flags      = flags;
        c.color      = color;
        c.z          = z;
        c.stencil    = stencil;
        chunk(D3D8CAP_CLEAR, &c, sizeof c, r, (size_t)n * sizeof r[0], NULL, 0);
    }
    return dev->lpVtbl->Clear(dev, count, rects, flags, color, z, stencil);
}

HRESULT host_Swap(IDirect3DDevice8 *dev, DWORD flags)
{
    return dev->lpVtbl->Swap(dev, flags);
}

HRESULT host_SetRenderState(IDirect3DDevice8 *dev, D3DRENDERSTATETYPE state, DWORD value)
{
    if (g_cap)
        rec_render_state((DWORD)state, value);
    return dev->lpVtbl->SetRenderState(dev, state, value);
}

HRESULT host_SetTextureStageState(IDirect3DDevice8 *dev, DWORD stage,
                                  D3DTEXTURESTAGESTATETYPE type, DWORD value)
{
    if (g_cap)
        rec_stage_state(stage, (DWORD)type, value);
    return dev->lpVtbl->SetTextureStageState(dev, stage, type, value);
}

HRESULT host_SetTransform(IDirect3DDevice8 *dev, D3DTRANSFORMSTATETYPE state,
                          const D3DMATRIX *matrix)
{
    if (g_cap && matrix)
        rec_transform((DWORD)state, matrix);
    return dev->lpVtbl->SetTransform(dev, state, matrix);
}

HRESULT host_SetViewport(IDirect3DDevice8 *dev, const D3DVIEWPORT8 *viewport)
{
    if (g_cap && viewport)
        rec_viewport(viewport);
    return dev->lpVtbl->SetViewport(dev, viewport);
}

HRESULT host_SetTexture(IDirect3DDevice8 *dev, DWORD stage, IDirect3DBaseTexture8 *texture)
{
    if (g_cap)
        rec_set_texture(stage, texture);
    return dev->lpVtbl->SetTexture(dev, stage, texture);
}

HRESULT host_SetVertexShader(IDirect3DDevice8 *dev, DWORD handle)
{
    if (g_cap)
        rec_set_vertex_shader(handle);
    return dev->lpVtbl->SetVertexShader(dev, handle);
}

HRESULT host_DrawPrimitiveUP(IDirect3DDevice8 *dev, D3DPRIMITIVETYPE type,
                             UINT prims, const void *vertices, UINT stride)
{
    if (g_cap && vertices && stride) {
        D3D8CapDrawUp c;
        uint64_t bytes = (uint64_t)d3d8_up_vertices_read(type, prims) * stride;

        if (bytes <= 0xFFFFFFFFu) {
            c.prim_type    = (uint32_t)type;
            c.prim_count   = prims;
            c.stride       = stride;
            c.vertex_bytes = (uint32_t)bytes;
            chunk(D3D8CAP_DRAW_UP, &c, sizeof c, vertices, (size_t)bytes, NULL, 0);
            g_draws++;
        }
    }
    return dev->lpVtbl->DrawPrimitiveUP(dev, type, prims, vertices, stride);
}

HRESULT host_DrawIndexedPrimitiveUP(IDirect3DDevice8 *dev, D3DPRIMITIVETYPE type,
                                    UINT min_index, UINT num_vertices, UINT prims,
                                    const void *indices, D3DFORMAT index_format,
                                    const void *vertices, UINT stride)
{
    if (g_cap && indices && vertices && stride) {
        D3D8CapDrawIndexedUp c;
        UINT index_size = index_format == D3DFMT_INDEX32 ? 4u : 2u;
        uint64_t ibytes = (uint64_t)d3d8_up_indices_read(type, prims) * index_size;
        uint64_t vbytes = (uint64_t)num_vertices * stride;

        if (ibytes <= 0xFFFFFFFFu && vbytes <= 0xFFFFFFFFu) {
            c.prim_type    = (uint32_t)type;
            c.min_index    = min_index;
            c.num_vertices = num_vertices;
            c.prim_count   = prims;
            c.index_format = (uint32_t)index_format;
            c.index_bytes  = (uint32_t)ibytes;
            c.stride       = stride;
            c.vertex_bytes = (uint32_t)vbytes;
            chunk(D3D8CAP_DRAW_INDEXED_UP, &c, sizeof c,
                  indices, (size_t)ibytes, vertices, (size_t)vbytes);
            g_draws++;
        }
    }
    return dev->lpVtbl->DrawIndexedPrimitiveUP(dev, type, min_index, num_vertices,
                                               prims, indices, index_format,
                                               vertices, stride);
}

HRESULT host_CreateTexture(IDirect3DDevice8 *dev, UINT width, UINT height,
                           UINT levels, DWORD usage, D3DFORMAT format,
                           D3DPOOL pool, IDirect3DTexture8 **texture)
{
    return dev->lpVtbl->CreateTexture(dev, width, height, levels, usage, format,
                                      pool, texture);
}

HRESULT host_CreateCubeTexture(IDirect3DDevice8 *dev, UINT edge, UINT levels,
                               DWORD usage, D3DFORMAT format, D3DPOOL pool,
                               IDirect3DCubeTexture8 **texture)
{
    return dev->lpVtbl->CreateCubeTexture(dev, edge, levels, usage, format,
                                          pool, texture);
}

HRESULT host_LockRect(IDirect3DTexture8 *texture, UINT level,
                      D3DLOCKED_RECT *locked, const RECT *rect, DWORD flags)
{
    return texture->lpVtbl->LockRect(texture, level, locked, rect, flags);
}

HRESULT host_UnlockRect(IDirect3DTexture8 *texture, UINT level)
{
    HRESULT hr = texture->lpVtbl->UnlockRect(texture, level);
    IDirect3DBaseTexture8 *object = (IDirect3DBaseTexture8 *)texture;
    int i;

    /* A texture the capture does not hold yet is written whole when it is
     * first bound, with whatever this unlock left in it. */
    if (g_cap && SUCCEEDED(hr) && (i = texture_find(object)) >= 0) {
        D3D8CapTextureLevel c;
        const BYTE *bits;
        UINT pitch, rows;

        if (d3d8_texture_level(object, level, &bits, &pitch, &rows)) {
            c.id    = g_textures[i].id;
            c.level = level;
            c.pitch = pitch;
            c.rows  = rows;
            c.bytes = pitch * rows;
            chunk(D3D8CAP_TEXTURE_LEVEL, &c, sizeof c, bits, c.bytes, NULL, 0);
            g_level_writes++;
            g_texture_bytes += c.bytes;
        }
    }
    return hr;
}

ULONG host_ReleaseTexture(IDirect3DTexture8 *texture)
{
    IDirect3DBaseTexture8 *object = (IDirect3DBaseTexture8 *)texture;
    ULONG left = texture->lpVtbl->Release(texture);
    int i;

    /* Only the pointer is used from here on, as a key: the object may be gone. */
    if (left == 0 && (IDirect3DBaseTexture8 *)texture == g_target_texture) {
        g_target_texture = NULL;
        g_target_lost = 1;
    }
    if (g_cap && left == 0 && (i = texture_find(object)) >= 0) {
        D3D8CapTextureId c;

        c.id = g_textures[i].id;
        chunk(D3D8CAP_TEXTURE_RELEASE, &c, sizeof c, NULL, 0, NULL, 0);
        g_textures[i] = g_textures[--g_texture_count];
    }
    return left;
}

/* ------------------------------------------------------- render targets */

HRESULT host_SetRenderTarget(IDirect3DDevice8 *dev, IDirect3DBaseTexture8 *texture,
                             UINT level, UINT face, IDirect3DSurface8 *depth)
{
    IDirect3DSurface8 *surface = NULL;
    D3D8CubeInfo cube;
    HRESULT hr;

    if (texture && d3d8_cube_info(texture, &cube)) {
        IDirect3DCubeTexture8 *c = (IDirect3DCubeTexture8 *)texture;

        hr = c->lpVtbl->GetCubeMapSurface(c, (D3DCUBEMAP_FACES)face, level, &surface);
        if (FAILED(hr) || !surface)
            return FAILED(hr) ? hr : E_FAIL;
    } else if (texture) {
        IDirect3DTexture8 *t = (IDirect3DTexture8 *)texture;

        hr = t->lpVtbl->GetSurfaceLevel(t, level, &surface);
        if (FAILED(hr) || !surface)
            return FAILED(hr) ? hr : E_FAIL;
    }
    hr = dev->lpVtbl->SetRenderTarget(dev, surface, depth);
    if (surface)
        surface->lpVtbl->Release(surface);   /* the device holds its own reference */
    /* The call is recorded as made either way: replay repeats it, and a
     * refusal with it. */
    g_target_texture = texture;
    g_target_level   = level;
    g_target_face    = face;
    g_target_depth   = depth;
    g_target_lost    = FAILED(hr);
    if (g_cap)
        rec_set_render_target(texture, level, face, depth, 0);
    return hr;
}

IDirect3DSurface8 *host_DeviceDepthSurface(IDirect3DDevice8 *dev)
{
    IDirect3DSurface8 *s = NULL;

    if (!g_device_depth && SUCCEEDED(dev->lpVtbl->GetDepthStencilSurface(dev, &s)) && s) {
        g_device_depth = s;
        g_target_depth = s;              /* what the device starts with */
        s->lpVtbl->Release(s);           /* the device keeps it alive */
    }
    return g_device_depth;
}

HRESULT host_CreateDepthStencilSurface(IDirect3DDevice8 *dev, UINT width, UINT height,
                                       D3DFORMAT format, IDirect3DSurface8 **surface)
{
    return dev->lpVtbl->CreateDepthStencilSurface(dev, width, height, format,
                                                  D3DMULTISAMPLE_NONE, surface);
}

/* -------------------------------------------- vertex programs, combiners */

HRESULT host_vsh_create_shader(const DWORD *microcode, int insn_count, DWORD *handle)
{
    HRESULT hr = d3d8_vsh_create_shader(microcode, insn_count, handle);

    /* Only a program that exists can be selected, so only those are kept.
     * The host truncates past NV2A_VS_MAX_INSTRUCTIONS; so does the record. */
    if (g_cap && SUCCEEDED(hr)) {
        if (insn_count > NV2A_VS_MAX_INSTRUCTIONS)
            insn_count = NV2A_VS_MAX_INSTRUCTIONS;
        rec_vs_create(*handle, microcode, insn_count);
    }
    return hr;
}

BOOL host_vsh_same_microcode(DWORD handle, const DWORD *microcode, int insn_count)
{
    const DWORD *have;
    int length;

    if (insn_count > NV2A_VS_MAX_INSTRUCTIONS)
        insn_count = NV2A_VS_MAX_INSTRUCTIONS;
    if (!d3d8_vsh_get_slot((int)(handle - 0x10000), NULL, &have, &length, NULL, NULL))
        return FALSE;
    return length == insn_count &&
           memcmp(have, microcode, (size_t)insn_count * 4 * sizeof(DWORD)) == 0;
}

HRESULT host_vsh_delete_shader(DWORD handle)
{
    if (g_cap) {
        D3D8CapVsHandle c;

        c.handle = handle;
        chunk(D3D8CAP_VS_DELETE, &c, sizeof c, NULL, 0, NULL, 0);
    }
    return d3d8_vsh_delete_shader(handle);
}

void host_vsh_set_constant(int first_reg, const float *data, int count)
{
    if (g_cap)
        rec_vs_constants(first_reg, data, count);
    d3d8_vsh_set_constant(first_reg, data, count);
}

HRESULT host_vsh_set_declaration(DWORD handle, const D3D8VshInput *inputs, int count)
{
    if (g_cap)
        rec_vs_declaration(handle, inputs, count);
    return d3d8_vsh_set_declaration(handle, inputs, count);
}

void host_vsh_set_screenspace(const float scale[4], const float offset[4])
{
    if (g_cap && scale && offset)
        rec_vs_screenspace(1, scale, offset);
    d3d8_vsh_set_screenspace(scale, offset);
}

void host_vsh_set_vertex_data(int reg, const float value[4])
{
    if (g_cap && value)
        rec_vs_vertex_data(reg, value);
    d3d8_vsh_set_vertex_data(reg, value);
}

void host_combiners_set_pixel_shader(DWORD token)
{
    if (g_cap)
        rec_ps_token(token);
    d3d8_combiners_set_pixel_shader(token);
}

#endif /* _WIN32 */
