/*
 * hle_d3d8_texture.c -- D3DDevice_SetTexture, forwarded to the shadow device.
 *
 * Part of shadow mode (hle_d3d8.c, RECOMP_HLE_D3D8=shadow). The title's own
 * SetTexture runs first; then the texture it bound is found or built on the
 * host device and bound to the same stage there.
 *
 * An Xbox texture is a guest X_D3DPixelContainer (Cxbx-Reloaded,
 * XbD3D8Types.h): Common, Data, Lock, Format, Size. Data is a physical
 * address; the runtime keeps physical page P at guest 0x80000000 + P, where
 * MmAllocateContiguousMemory allocations live (xbox_memory_layout.c), so the
 * texels are read there. Format packs the D3DFORMAT (bits 8-15), mip levels
 * (16-19) and, for swizzled and compressed textures, log2 width and height
 * (20-23, 24-27). A linear texture instead has a non-zero Size: width-1,
 * height-1 and pitch/64-1, and one level (CxbxGetPixelContainerMeasures in
 * XbConvert.cpp).
 *
 * The host D3D8 layer takes Xbox format codes itself and keeps a mip chain in
 * the Xbox's own packing -- level after level, each row_pitch * rows -- which
 * it unswizzles and converts at upload. So swizzled and compressed levels are
 * copied as they are. That the guest packs its levels with no padding between
 * them is assumed, not verified.
 *
 * Titles write texels without any call the replacement could see (Lock is
 * inline), so a cached texture is checksummed again the first time it is
 * bound in each frame and uploaded again if it changed.
 *
 * Not handled, counted instead: cube and volume textures, P8 (no palette is
 * forwarded), surfaces bound as textures, textures outside the contiguous
 * window.
 */
#include "platform/xbox_winnt.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hle.h"

#ifdef _WIN32
#include "d3d8_xbox.h"
#include "d3d8_internal.h"
#include "hle_d3d8_capture.h"

/* From hle_d3d8.c: the shadow device, or NULL, and its swap count. */
IDirect3DDevice8 *hle_d3d8_shadow_device(void);
unsigned long hle_d3d8_shadow_swaps(void);

#define CONTIG_BASE          0x80000000u
#define CONTIG_SIZE          (64u * 1024u * 1024u)   /* kernel.h XBOX_CONTIG_SIZE */
#define COMMON_TYPE_MASK     0x00070000u
#define COMMON_TYPE_TEXTURE  0x00040000u
#define FORMAT_CUBEMAP       0x00000004u
#define XFMT_P8              0x0B
#define TEXTURE_CACHE        512
#define MAX_STAGES           4

typedef struct {
    uint32_t           va, data, format, size;   /* guest identity */
    IDirect3DTexture8 *host;
    uint32_t           checksum;
    unsigned long      checked_swap, used_swap;
} texture_entry;

static texture_entry g_textures[TEXTURE_CACHE];
static int           g_texture_count;
static IDirect3DTexture8 *g_bound[MAX_STAGES];
/* The guest container behind each stage's host texture, so a frame capture
 * that begins after the title bound its textures can re-read their texels. */
static uint32_t      g_bound_va[MAX_STAGES];

static unsigned long g_bound_count, g_uploads, g_reuploads, g_skip_type,
                     g_skip_cube, g_skip_format, g_skip_range, g_skip_create;

typedef struct {
    uint32_t fmt, width, height, levels, guest_pitch;
    int      linear;                 /* Size != 0: one level, guest pitch */
    uint32_t phys, bytes;            /* where the texels are, and how many */
} texture_layout;

static uint32_t level_dim(uint32_t d, uint32_t level)
{
    d >>= level;
    return d ? d : 1;
}

static uint32_t level_rows(uint32_t fmt, uint32_t h)
{
    return d3d8_format_is_compressed((D3DFORMAT)fmt) ? (h + 3) / 4 : h;
}

/* Reads the guest pixel container. 0 with a skip counter bumped if the
 * texture is one this file does not forward. */
static int read_layout(uint32_t va, texture_layout *t)
{
    uint32_t common = HLE_MEM32(va + 0);
    uint32_t data   = HLE_MEM32(va + 4);
    uint32_t format = HLE_MEM32(va + 12);
    uint32_t size   = HLE_MEM32(va + 16);
    uint32_t l;
    uint64_t bytes = 0;

    if ((common & COMMON_TYPE_MASK) != COMMON_TYPE_TEXTURE) {
        g_skip_type++;
        return 0;
    }
    if ((format & FORMAT_CUBEMAP) || ((format >> 4) & 0xF) != 2) {
        g_skip_cube++;
        return 0;
    }
    memset(t, 0, sizeof *t);
    t->fmt = (format >> 8) & 0xFF;
    if (t->fmt == XFMT_P8 || d3d8_format_bpp((D3DFORMAT)t->fmt) == 0) {
        g_skip_format++;
        return 0;
    }
    if (size) {
        t->linear = 1;
        t->width  = (size & 0xFFF) + 1;
        t->height = ((size >> 12) & 0xFFF) + 1;
        t->guest_pitch = (((size >> 24) & 0xFF) + 1) * 64;
        t->levels = 1;
        if (t->guest_pitch < d3d8_row_pitch((D3DFORMAT)t->fmt, t->width)) {
            g_skip_format++;
            return 0;
        }
        bytes = (uint64_t)t->guest_pitch * level_rows(t->fmt, t->height);
    } else {
        t->width  = 1u << ((format >> 20) & 0xF);
        t->height = 1u << ((format >> 24) & 0xF);
        t->levels = (format >> 16) & 0xF;
        if (!t->levels)
            t->levels = 1;
        for (l = 0; l < t->levels; l++)
            bytes += (uint64_t)d3d8_row_pitch((D3DFORMAT)t->fmt, level_dim(t->width, l)) *
                     level_rows(t->fmt, level_dim(t->height, l));
    }
    /* Data is physical; tolerate it arriving with the window's high bits. */
    t->phys = data & 0x0FFFFFFF;
    if (!data || (uint64_t)t->phys + bytes > CONTIG_SIZE) {
        g_skip_range++;
        return 0;
    }
    t->bytes = (uint32_t)bytes;
    return 1;
}

/* FNV-1a over level 0, or over 4096 evenly spaced bytes of a large one. A
 * sampled checksum can miss a small change; it is the price of checking every
 * bound texture once per frame. */
static uint32_t level0_checksum(const texture_layout *t)
{
    const uint8_t *p = (const uint8_t *)HLE_PTR(CONTIG_BASE + t->phys);
    uint32_t n = t->linear
        ? t->guest_pitch * level_rows(t->fmt, t->height)
        : d3d8_row_pitch((D3DFORMAT)t->fmt, t->width) * level_rows(t->fmt, t->height);
    uint32_t h = 2166136261u, i, step = n > 4096 ? n / 4096 : 1;

    for (i = 0; i < n; i += step) {
        h ^= p[i];
        h *= 16777619u;
    }
    return h;
}

static void upload(IDirect3DTexture8 *host, const texture_layout *t)
{
    const uint8_t *src = (const uint8_t *)HLE_PTR(CONTIG_BASE + t->phys);
    uint32_t l, y;

    for (l = 0; l < t->levels; l++) {
        uint32_t w = level_dim(t->width, l), h = level_dim(t->height, l);
        uint32_t pitch = d3d8_row_pitch((D3DFORMAT)t->fmt, w);
        uint32_t rows = level_rows(t->fmt, h);
        D3DLOCKED_RECT lr;

        if (FAILED(host->lpVtbl->LockRect(host, l, &lr, NULL, 0)))
            return;
        if (t->linear) {             /* guest rows are padded to 64 bytes */
            for (y = 0; y < rows; y++)
                memcpy((uint8_t *)lr.pBits + (size_t)y * (size_t)lr.Pitch,
                       src + (size_t)y * t->guest_pitch, pitch);
        } else {
            memcpy(lr.pBits, src, (size_t)pitch * rows);
            src += (size_t)pitch * rows;
        }
        host->lpVtbl->UnlockRect(host, l);
    }
}

static void note_format(const texture_layout *t)
{
    static uint32_t seen[32];
    static int nseen;
    uint32_t key = t->fmt | (t->levels << 8) | ((uint32_t)t->linear << 12);
    int i;

    for (i = 0; i < nseen; i++)
        if (seen[i] == key)
            return;
    if (nseen < (int)(sizeof seen / sizeof seen[0])) {
        seen[nseen++] = key;
        fprintf(stderr, "[HLE-D3D8] shadow texture: format 0x%02X %ux%u, %u level(s), %s\n",
                t->fmt, t->width, t->height, t->levels,
                t->linear ? "linear" : "swizzled/compressed");
    }
}

/* Frees the least recently bound entry that no stage holds. */
static texture_entry *cache_slot(IDirect3DDevice8 *dev, unsigned long now)
{
    texture_entry *victim = NULL;
    int i, s;

    if (g_texture_count < TEXTURE_CACHE)
        return &g_textures[g_texture_count++];
    for (i = 0; i < TEXTURE_CACHE; i++) {
        texture_entry *e = &g_textures[i];
        int held = 0;
        for (s = 0; s < MAX_STAGES; s++)
            held |= g_bound[s] == e->host;
        if (!held && e->used_swap != now && (!victim || e->used_swap < victim->used_swap))
            victim = e;
    }
    if (!victim)
        return NULL;
    (void)dev;
    if (victim->host)
        victim->host->lpVtbl->Release(victim->host);
    memset(victim, 0, sizeof *victim);
    return victim;
}

static IDirect3DTexture8 *host_texture(IDirect3DDevice8 *dev, uint32_t va)
{
    unsigned long now = hle_d3d8_shadow_swaps();
    texture_layout t;
    texture_entry *e = NULL;
    uint32_t data = HLE_MEM32(va + 4), format = HLE_MEM32(va + 12), size = HLE_MEM32(va + 16);
    int i;

    for (i = 0; i < g_texture_count; i++) {
        texture_entry *c = &g_textures[i];
        if (c->host && c->va == va && c->data == data && c->format == format && c->size == size) {
            e = c;
            break;
        }
    }
    if (e) {
        e->used_swap = now;
        if (e->checked_swap != now) {
            e->checked_swap = now;
            if (read_layout(va, &t)) {
                uint32_t sum = level0_checksum(&t);
                if (sum != e->checksum) {
                    e->checksum = sum;
                    upload(e->host, &t);
                    g_reuploads++;
                }
            }
        }
        return e->host;
    }

    if (!read_layout(va, &t))
        return NULL;
    note_format(&t);
    e = cache_slot(dev, now);
    if (!e)
        return NULL;
    if (FAILED(dev->lpVtbl->CreateTexture(dev, t.width, t.height, t.levels, 0,
                                          (D3DFORMAT)t.fmt, D3DPOOL_MANAGED, &e->host)) ||
        !e->host) {
        memset(e, 0, sizeof *e);
        g_skip_create++;
        return NULL;
    }
    e->va = va;
    e->data = data;
    e->format = format;
    e->size = size;
    e->checksum = level0_checksum(&t);
    e->checked_swap = e->used_swap = now;
    upload(e->host, &t);
    g_uploads++;
    return e->host;
}

/* 1x1 opaque white, created once, never evicted. */
static IDirect3DTexture8 *white_texture(IDirect3DDevice8 *dev)
{
    static IDirect3DTexture8 *white;
    static int tried;
    D3DLOCKED_RECT lr;

    if (white || tried)
        return white;
    tried = 1;
    if (FAILED(dev->lpVtbl->CreateTexture(dev, 1, 1, 1, 0, D3DFMT_LIN_A8R8G8B8,
                                          D3DPOOL_MANAGED, &white)) || !white) {
        white = NULL;
        return NULL;
    }
    if (SUCCEEDED(white->lpVtbl->LockRect(white, 0, &lr, NULL, 0))) {
        memset(lr.pBits, 0xFF, 4);
        white->lpVtbl->UnlockRect(white, 0);
    }
    return white;
}

/* Records what is bound to one stage. read_layout is called again rather than
 * threaded through host_texture: on the path that reaches here it has already
 * succeeded once, and it bumps its skip counters only when it fails, so this
 * second call cannot double-count. */
static void capture_stage(uint32_t stage, uint32_t va)
{
    texture_layout t;
    HleD3D8CaptureTexture c;

    if (!hle_d3d8_capture_active())
        return;
    if (!va || !read_layout(va, &t)) {
        hle_d3d8_capture_unbind_texture(stage);
        return;
    }
    c.guest_va    = va;
    c.format      = t.fmt;
    c.width       = t.width;
    c.height      = t.height;
    c.levels      = t.levels;
    c.linear      = t.linear;
    c.guest_pitch = t.guest_pitch;
    c.texels      = HLE_PTR(CONTIG_BASE + t.phys);
    hle_d3d8_capture_bind_texture(stage, &c);
}

/* Called by the capture at frame start: every stage as it stands, so a replay
 * starts from the same bindings rather than from an empty device. */
void hle_d3d8_capture_snapshot_textures(void)
{
    uint32_t s;

    for (s = 0; s < MAX_STAGES; s++)
        capture_stage(s, g_bound_va[s]);
}

static void report(void)
{
    static DWORD last;
    DWORD now = GetTickCount();

    if (!last) {
        last = now;
    } else if (now - last >= 5000) {
        fprintf(stderr, "[HLE-D3D8] shadow textures: %lu binds, %d cached, %lu uploads, "
                "%lu re-uploads; skipped %lu not a texture, %lu cube/volume, %lu format, "
                "%lu out of range, %lu create failed\n",
                g_bound_count, g_texture_count, g_uploads, g_reuploads, g_skip_type,
                g_skip_cube, g_skip_format, g_skip_range, g_skip_create);
        last = now;
    }
}
#endif /* _WIN32 */

HLE_ORIGINAL(D3DDevice_SetTexture);

/* HRESULT D3DDevice_SetTexture(DWORD Stage, IDirect3DBaseTexture8 *pTexture) */
HLE_EXPORT(D3DDevice_SetTexture)
{
    static int seen;
    uint32_t stage = HLE_ARG(0);
#ifdef _WIN32
    uint32_t texture = HLE_ARG(1);
#endif

    if (!seen) {
        seen = 1;
        fprintf(stderr, "[HLE] D3DDevice_SetTexture(0x%X) replaced by name\n", stage);
    }
    if (!hle_original_D3DDevice_SetTexture) {
        fprintf(stderr, "[HLE] D3DDevice_SetTexture: original body missing -- regenerate the lift\n");
        HLE_RETURN(0x80004005u);
    }
    HLE_CALL_ORIGINAL(D3DDevice_SetTexture);
#ifdef _WIN32
    {
        IDirect3DDevice8 *dev = hle_d3d8_shadow_device();
        IDirect3DTexture8 *host = NULL;

        if (!dev || stage >= MAX_STAGES)
            return;
        if (texture)
            host = host_texture(dev, texture);
        g_bound[stage] = host;
        g_bound_va[stage] = host ? texture : 0;
        capture_stage(stage, g_bound_va[stage]);
        /* The host pixel shader samples every stage whatever its operation
         * (d3d8_shaders.c), and an unbound D3D11 slot reads as zero, so a
         * stage with no host texture would turn the draw black once the
         * title's own texture operations are forwarded. It gets opaque white
         * instead. That a missing Xbox texture contributes white is assumed,
         * not verified against hardware. */
        dev->lpVtbl->SetTexture(dev, stage,
                                (IDirect3DBaseTexture8 *)(host ? host : white_texture(dev)));
        g_bound_count++;
        report();
    }
#endif
}
