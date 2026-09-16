/*
 * hle_d3d8_capture.c -- record one frame of shadow mode to a capture file.
 *
 * Part of shadow mode (hle_d3d8.c, RECOMP_HLE_D3D8=shadow).
 *
 *   RECOMP_D3D8_CAPTURE=<path>     where to write; capture is off without it
 *   RECOMP_D3D8_CAPTURE_SWAP=<n>   which swap to record (default 120)
 *
 * The default is 120 because the first frames of a title are its loader:
 * nothing is bound, most state is still at the device's defaults, and the
 * capture would be of an empty screen. 120 swaps is far enough in to have
 * textures and a real draw list, and still inside the first minute of a run.
 *
 * What a capture holds and what it deliberately leaves out is documented in
 * d3d8_capture.h. The one thing worth repeating here: a title that fills its
 * own push buffer through BeginPush bypasses every replacement, so those
 * draws are not in the capture and a replay of such a frame is incomplete
 * without saying so. Burnout 2 has two such call sites (hle_d3d8.c).
 *
 * Frame boundaries: recording starts when the swap counter reaches the
 * requested swap and stops at the next one, so the file holds the operations
 * between one Swap and the next. Most of the state those operations draw
 * with was set in earlier frames, so at frame start every part of shadow mode
 * is asked to re-emit what it holds -- see the snapshot calls below. Without
 * that a capture would replay against a device at its defaults and look
 * nothing like the run.
 */
#include "platform/xbox_winnt.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hle.h"

#ifdef _WIN32

#include "d3d8_xbox.h"
#include "d3d8_internal.h"
#include "d3d8_vsh.h"
#include "d3d8_capture.h"
#include "hle_d3d8_capture.h"

#define CAPTURE_DEFAULT_SWAP  120
#define CAPTURE_MAX_TEXTURES  512
#define CAPTURE_MAX_STATES     64
#define CAPTURE_STAGES          4
#define CAPTURE_MAX_LEVELS     16

static D3D8CapWriter *g_cap;
static int            g_configured;
static const char    *g_path;
static unsigned long  g_target_swap;
static unsigned long  g_frame;

/* Pending state batch for the draw about to be made. */
static D3D8CapStatePair g_rs[CAPTURE_MAX_STATES];
static uint32_t         g_rs_count;
static D3D8CapStatePair g_ts[CAPTURE_STAGES][CAPTURE_MAX_STATES];
static uint32_t         g_ts_count[CAPTURE_STAGES];

/* Textures already written to this capture, so one bound by many draws is
 * stored once. Keyed by the guest container's address together with the
 * fields that decide its contents: a title that frees a texture and puts a
 * different one at the same address inside one frame would otherwise reuse
 * the first one's bytes. */
static struct {
    uint32_t va, format, width, height, levels, id;
} g_textures[CAPTURE_MAX_TEXTURES];
static int      g_texture_count;
static uint32_t g_next_texture_id = 1;

static unsigned long g_draws, g_dropped_textures;

int hle_d3d8_capture_active(void)
{
    return g_cap != NULL;
}

static void capture_configure(void)
{
    const char *swap;

    g_configured = 1;
    g_path = getenv("RECOMP_D3D8_CAPTURE");
    g_target_swap = CAPTURE_DEFAULT_SWAP;
    swap = getenv("RECOMP_D3D8_CAPTURE_SWAP");
    if (swap && atol(swap) > 0)
        g_target_swap = (unsigned long)atol(swap);
    if (g_path && *g_path)
        fprintf(stderr, "[HLE-D3D8] capture armed: swap %lu -> %s\n",
                g_target_swap, g_path);
}

/* Everything the frame draws with that was set before it began. */
static void capture_frame_start(void)
{
    const float *constants = d3d8_vsh_constants();

    /* The whole 192-register bank, so a replay does not depend on constants
     * set in earlier frames. Updates inside the frame append to this. */
    if (constants) {
        D3D8CapVsConstants c;

        c.first_reg = 0;
        c.count     = NV2A_VS_MAX_CONSTANTS;
        d3d8cap_chunk(g_cap, D3D8CAP_VS_CONSTANTS, &c, sizeof c,
                      constants, (size_t)NV2A_VS_MAX_CONSTANTS * 4u * sizeof(float),
                      NULL, 0);
    }
    hle_d3d8_capture_snapshot_shader();
    hle_d3d8_capture_snapshot_textures();
    /* Render and texture stage states arrive with the first draw, once this
     * has made hle_d3d8_state.c forget what it last applied. */
    hle_d3d8_shadow_states_invalidate();
}

void hle_d3d8_capture_swap(unsigned long swaps, uint32_t width, uint32_t height)
{
    if (!g_configured)
        capture_configure();
    if (!g_path || !*g_path)
        return;

    if (g_cap) {
        int ok;
        uint32_t chunks = d3d8cap_chunk_count(g_cap);

        ok = d3d8cap_close(g_cap) == 0;
        g_cap = NULL;
        fprintf(stderr, "[HLE-D3D8] capture: frame %lu written to %s -- %u chunks, "
                "%lu draws, %d textures%s\n", g_frame, g_path, chunks, g_draws,
                g_texture_count, ok ? "" : " (INCOMPLETE: write failed)");
        if (g_dropped_textures)
            fprintf(stderr, "[HLE-D3D8] capture: %lu texture binds not recorded "
                    "(table full)\n", g_dropped_textures);
        fflush(stderr);
        return;
    }
    if (swaps != g_target_swap)
        return;

    g_frame = swaps;
    g_cap = d3d8cap_create(g_path, (uint32_t)swaps, width, height);
    if (!g_cap) {
        fprintf(stderr, "[HLE-D3D8] capture: cannot write %s; capture off\n", g_path);
        g_path = NULL;                   /* one attempt, not one per swap */
        return;
    }
    capture_frame_start();
}

void hle_d3d8_capture_clear(uint32_t flags, uint32_t color,
                            uint32_t z_bits, uint32_t stencil)
{
    D3D8CapClear c;

    if (!g_cap)
        return;
    c.flags   = flags;
    c.color   = color;
    c.z_bits  = z_bits;
    c.stencil = stencil;
    d3d8cap_chunk(g_cap, D3D8CAP_CLEAR, &c, sizeof c, NULL, 0, NULL, 0);
}

void hle_d3d8_capture_transform(uint32_t xbox_state, const float *matrix16)
{
    D3D8CapTransform t;

    if (!g_cap || !matrix16)
        return;
    t.state = xbox_state;
    memcpy(t.m, matrix16, sizeof t.m);
    d3d8cap_chunk(g_cap, D3D8CAP_TRANSFORM, &t, sizeof t, NULL, 0, NULL, 0);
}

void hle_d3d8_capture_viewport(uint32_t x, uint32_t y, uint32_t width,
                               uint32_t height, float min_z, float max_z)
{
    D3D8CapViewport v;

    if (!g_cap)
        return;
    v.x = x;
    v.y = y;
    v.width = width;
    v.height = height;
    v.min_z = min_z;
    v.max_z = max_z;
    d3d8cap_chunk(g_cap, D3D8CAP_VIEWPORT, &v, sizeof v, NULL, 0, NULL, 0);
}

void hle_d3d8_capture_render_state(uint32_t host_state, uint32_t value)
{
    if (!g_cap || g_rs_count >= CAPTURE_MAX_STATES)
        return;
    g_rs[g_rs_count].state = host_state;
    g_rs[g_rs_count].value = value;
    g_rs_count++;
}

void hle_d3d8_capture_stage_state(uint32_t stage, uint32_t host_state, uint32_t value)
{
    if (!g_cap || stage >= CAPTURE_STAGES || g_ts_count[stage] >= CAPTURE_MAX_STATES)
        return;
    g_ts[stage][g_ts_count[stage]].state = host_state;
    g_ts[stage][g_ts_count[stage]].value = value;
    g_ts_count[stage]++;
}

void hle_d3d8_capture_states_flush(void)
{
    uint32_t stage;

    if (!g_cap)
        return;
    if (g_rs_count) {
        D3D8CapStateBatch b;

        b.count = g_rs_count;
        d3d8cap_chunk(g_cap, D3D8CAP_RENDER_STATE, &b, sizeof b,
                      g_rs, (size_t)g_rs_count * sizeof g_rs[0], NULL, 0);
        g_rs_count = 0;
    }
    for (stage = 0; stage < CAPTURE_STAGES; stage++) {
        D3D8CapStageBatch b;

        if (!g_ts_count[stage])
            continue;
        b.stage = stage;
        b.count = g_ts_count[stage];
        d3d8cap_chunk(g_cap, D3D8CAP_TEXTURE_STAGE_STATE, &b, sizeof b,
                      g_ts[stage], (size_t)g_ts_count[stage] * sizeof g_ts[stage][0],
                      NULL, 0);
        g_ts_count[stage] = 0;
    }
}

void hle_d3d8_capture_vs_program(uint32_t guest_handle,
                                 const uint32_t *microcode, uint32_t insn_count,
                                 const uint32_t *declaration, uint32_t decl_dwords)
{
    D3D8CapVsProgram p;

    if (!g_cap || !microcode || !insn_count)
        return;
    p.guest_handle = guest_handle;
    p.insn_count   = insn_count;
    p.decl_dwords  = declaration ? decl_dwords : 0;
    d3d8cap_chunk(g_cap, D3D8CAP_VS_PROGRAM, &p, sizeof p,
                  microcode, (size_t)insn_count * 4u * sizeof(uint32_t),
                  p.decl_dwords ? declaration : NULL,
                  (size_t)p.decl_dwords * sizeof(uint32_t));
}

void hle_d3d8_capture_vs_select(uint32_t guest_handle)
{
    D3D8CapVsSelect s;

    if (!g_cap)
        return;
    s.guest_handle = guest_handle;
    d3d8cap_chunk(g_cap, D3D8CAP_VS_SELECT, &s, sizeof s, NULL, 0, NULL, 0);
}

void hle_d3d8_capture_vs_constants(uint32_t first_reg, const float *data,
                                   uint32_t count)
{
    D3D8CapVsConstants c;

    if (!g_cap || !data || !count || first_reg >= NV2A_VS_MAX_CONSTANTS)
        return;
    if (count > (uint32_t)NV2A_VS_MAX_CONSTANTS - first_reg)
        count = (uint32_t)NV2A_VS_MAX_CONSTANTS - first_reg;
    c.first_reg = first_reg;
    c.count     = count;
    d3d8cap_chunk(g_cap, D3D8CAP_VS_CONSTANTS, &c, sizeof c,
                  data, (size_t)count * 4u * sizeof(float), NULL, 0);
}

/* ---------------------------------------------------------------- textures */

static uint32_t level_dim(uint32_t d, uint32_t level)
{
    d >>= level;
    return d ? d : 1;
}

static uint32_t level_rows(uint32_t fmt, uint32_t h)
{
    return d3d8_format_is_compressed((D3DFORMAT)fmt) ? (h + 3) / 4 : h;
}

/* Already in this capture? Returns its id, or 0. */
static uint32_t texture_known(const HleD3D8CaptureTexture *t)
{
    int i;

    for (i = 0; i < g_texture_count; i++)
        if (g_textures[i].va == t->guest_va && g_textures[i].format == t->format &&
            g_textures[i].width == t->width && g_textures[i].height == t->height &&
            g_textures[i].levels == t->levels)
            return g_textures[i].id;
    return 0;
}

/* Writes the texture and returns its id, or 0 if it could not be recorded.
 *
 * The level walk mirrors hle_d3d8_texture.c's upload(): levels are packed one
 * after another with no padding between them, each level_rows * row_pitch.
 * Storing each level's pitch and row count rather than recomputing them at
 * replay means a later change to d3d8_row_pitch cannot silently
 * re-interpret an existing capture. */
static uint32_t texture_write(const HleD3D8CaptureTexture *t)
{
    D3D8CapTexture head;
    D3D8CapTextureLevel levels[CAPTURE_MAX_LEVELS];
    const uint8_t *src = t->texels;
    size_t total = 0;
    uint32_t l, count = t->levels;

    if (!src || !count || count > CAPTURE_MAX_LEVELS)
        return 0;
    if (g_texture_count >= CAPTURE_MAX_TEXTURES) {
        g_dropped_textures++;
        return 0;
    }

    for (l = 0; l < count; l++) {
        uint32_t w = level_dim(t->width, l), h = level_dim(t->height, l);
        uint32_t pitch = t->linear ? t->guest_pitch
                                   : d3d8_row_pitch((D3DFORMAT)t->format, w);
        uint32_t rows = level_rows(t->format, h);

        levels[l].pitch = pitch;
        levels[l].rows  = rows;
        levels[l].bytes = pitch * rows;
        total += levels[l].bytes;
    }

    head.id     = g_next_texture_id;
    head.format = t->format;
    head.width  = t->width;
    head.height = t->height;
    head.levels = count;
    head.linear = (uint32_t)(t->linear != 0);
    if (d3d8cap_chunk(g_cap, D3D8CAP_TEXTURE, &head, sizeof head,
                      levels, (size_t)count * sizeof levels[0], src, total) != 0)
        return 0;

    g_textures[g_texture_count].va     = t->guest_va;
    g_textures[g_texture_count].format = t->format;
    g_textures[g_texture_count].width  = t->width;
    g_textures[g_texture_count].height = t->height;
    g_textures[g_texture_count].levels = t->levels;
    g_textures[g_texture_count].id     = head.id;
    g_texture_count++;
    return g_next_texture_id++;
}

static void emit_bind(uint32_t stage, uint32_t id)
{
    D3D8CapSetTexture b;

    b.stage      = stage;
    b.texture_id = id;
    d3d8cap_chunk(g_cap, D3D8CAP_SET_TEXTURE, &b, sizeof b, NULL, 0, NULL, 0);
}

void hle_d3d8_capture_bind_texture(uint32_t stage, const HleD3D8CaptureTexture *t)
{
    uint32_t id;

    if (!g_cap || !t)
        return;
    id = texture_known(t);
    if (!id)
        id = texture_write(t);
    emit_bind(stage, id);
}

void hle_d3d8_capture_unbind_texture(uint32_t stage)
{
    if (g_cap)
        emit_bind(stage, 0);
}

/* ------------------------------------------------------------------- draws */

void hle_d3d8_capture_draw_up(uint32_t prim, uint32_t vertex_count,
                              const void *vertices, uint32_t stride)
{
    D3D8CapDrawUp d;

    if (!g_cap || !vertices || !vertex_count || !stride)
        return;
    d.prim         = prim;
    d.vertex_count = vertex_count;
    d.stride       = stride;
    d.vertex_bytes = vertex_count * stride;
    d3d8cap_chunk(g_cap, D3D8CAP_DRAW_UP, &d, sizeof d,
                  vertices, d.vertex_bytes, NULL, 0);
    g_draws++;
}

void hle_d3d8_capture_draw_indexed_up(uint32_t prim, uint32_t index_count,
                                      const uint16_t *indices,
                                      const void *vertices, uint32_t stride,
                                      uint32_t vertex_bytes)
{
    D3D8CapDrawIndexedUp d;

    if (!g_cap || !indices || !vertices || !index_count || !stride)
        return;
    d.prim         = prim;
    d.index_count  = index_count;
    d.stride       = stride;
    d.vertex_bytes = vertex_bytes;
    d.index_bytes  = index_count * (uint32_t)sizeof(uint16_t);
    d3d8cap_chunk(g_cap, D3D8CAP_DRAW_INDEXED_UP, &d, sizeof d,
                  indices, d.index_bytes, vertices, vertex_bytes);
    g_draws++;
}

#endif /* _WIN32 */
