/*
 * d3d8_replay.c -- replay one captured frame into the host renderer.
 *
 * The point of the whole capture path: change the shader translation, the
 * combiners or the state conversion, rebuild, and look at the new image in
 * seconds instead of running the title for five minutes and driving its menus
 * to the moment of interest. A capture replays byte-identically every time,
 * so two images differ only where the code changed.
 *
 *   d3d8_replay <capture> [--out <prefix>] [--loops <n>] [--dump-every]
 *               [--hold] [--quiet]
 *
 *   --out <prefix>   BMP path prefix (default "replay"); files are
 *                    <prefix>NNN.bmp, in the same 24-bit format the shadow
 *                    path's RECOMP_HLE_D3D8_DUMP writes, so a replay image
 *                    and a live frame can be put side by side.
 *   --loops <n>      replay the frame n times (default 1). The device is
 *                    created once; each loop walks the capture again.
 *   --dump-every     write a BMP after every loop, not just the last, so a
 *                    frame that is not idempotent shows itself.
 *   --hold           leave the window up until it is closed.
 *
 * No game files, no recompiled title and no guest memory: the capture carries
 * every byte the frame reads. It links the host D3D8 layer (src/d3d) directly
 * and creates the device the same way hle_d3d8.c's shadow_create does.
 *
 * What replay does NOT reproduce, beyond what d3d8_capture.h already lists:
 *   - the vertex declaration that came with a program is read from the
 *     capture but not applied, because the host CreateVertexShader ignores
 *     pDeclaration (d3d8_device.c) and builds its input layout from the
 *     microcode's inputs instead. A capture carries it so that a replay can
 *     start using it without retaking captures.
 *   - a stage whose capture says "nothing bound" gets the same 1x1 opaque
 *     white texture hle_d3d8_texture.c binds, for the same reason: the host
 *     pixel shader samples every stage whatever its operation, and an unbound
 *     D3D11 slot reads as zero, which would turn the draw black.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "d3d8_xbox.h"
#include "d3d8_internal.h"
#include "d3d8_vsh.h"
#include "d3d8_capture.h"
#include "d3d8_xbox_map.h"

#define REPLAY_MAX_TEXTURES 512

static int g_quiet;

static void note(const char *fmt, ...)
{
    va_list ap;

    if (g_quiet)
        return;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

/* ------------------------------------------------------------------ window */

static LRESULT CALLBACK replay_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

/* Unlike shadow mode, replay is single-threaded: there are no guest threads,
 * so the window lives on the same thread that presents and is pumped between
 * loops. */
static HWND replay_window(UINT width, UINT height)
{
    WNDCLASSA wc;
    RECT r;

    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc   = replay_wndproc;
    wc.hInstance     = GetModuleHandleA(NULL);
    wc.hCursor       = LoadCursorA(NULL, MAKEINTRESOURCEA(32512));   /* IDC_ARROW */
    wc.lpszClassName = "xboxrecomp_d3d8_replay";
    RegisterClassA(&wc);

    r.left = 0;
    r.top = 0;
    r.right = (LONG)width;
    r.bottom = (LONG)height;
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    return CreateWindowA(wc.lpszClassName, "xboxrecomp - D3D8 frame replay",
                         WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                         CW_USEDEFAULT, CW_USEDEFAULT,
                         r.right - r.left, r.bottom - r.top,
                         NULL, NULL, wc.hInstance, NULL);
}

static void pump(void)
{
    MSG msg;

    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
}

/* -------------------------------------------------------------------- dump */

/* The same 24-bit BMP the shadow path writes (hle_d3d8.c, shadow_dump_frame),
 * so images from the two paths are directly comparable. */
static void dump_bmp(IDirect3DDevice8 *dev, const char *path, UINT w, UINT h)
{
    IDirect3DSurface8 *surf = NULL;
    D3DLOCKED_RECT lr;
    uint8_t hdr[54];
    UINT y, x, pad;
    uint32_t filesz;
    FILE *f;

    if (FAILED(dev->lpVtbl->GetBackBuffer(dev, 0, 0, &surf)) || !surf)
        return;
    if (FAILED(surf->lpVtbl->LockRect(surf, &lr, NULL, D3DLOCK_READONLY))) {
        surf->lpVtbl->Release(surf);
        return;
    }
    pad = (4 - ((w * 3) & 3)) & 3;
    filesz = 54 + (w * 3 + pad) * h;

    f = fopen(path, "wb");
    if (f) {
        memset(hdr, 0, sizeof hdr);
        hdr[0] = 'B'; hdr[1] = 'M';
        memcpy(hdr + 2, &filesz, 4);
        hdr[10] = 54;
        hdr[14] = 40;
        memcpy(hdr + 18, &w, 4);
        memcpy(hdr + 22, &h, 4);
        hdr[26] = 1;
        hdr[28] = 24;
        fwrite(hdr, 1, sizeof hdr, f);
        for (y = h; y-- > 0; ) {         /* BMP rows run bottom-up */
            const uint8_t *row = (const uint8_t *)lr.pBits + (size_t)y * (size_t)lr.Pitch;
            for (x = 0; x < w; x++) {
                const uint8_t *p = row + x * 4;
                uint8_t bgr[3] = { p[2], p[1], p[0] };
                fwrite(bgr, 1, 3, f);
            }
            fwrite("\0\0\0", 1, pad, f);
        }
        fclose(f);
        note("[replay] wrote %s\n", path);
    } else {
        fprintf(stderr, "[replay] cannot write %s\n", path);
    }
    surf->lpVtbl->UnlockRect(surf);
    surf->lpVtbl->Release(surf);
}

/* ------------------------------------------------------------ replay state */

typedef struct {
    IDirect3DDevice8  *dev;
    UINT               width, height;

    /* Capture texture id -> host texture. Index 0 is unused: id 0 means
     * "nothing bound". */
    IDirect3DTexture8 *textures[REPLAY_MAX_TEXTURES];
    IDirect3DTexture8 *white;

    /* Guest program handle -> host handle from d3d8_vsh_create_shader. */
    struct { uint32_t guest; DWORD host; } programs[128];
    int                program_count;

    unsigned long      draws, skipped;
} Replay;

/* 1x1 opaque white, for a stage the capture says has nothing bound --
 * hle_d3d8_texture.c does the same, and for the same reason. */
static IDirect3DTexture8 *white_texture(Replay *r)
{
    D3DLOCKED_RECT lr;

    if (r->white)
        return r->white;
    if (FAILED(r->dev->lpVtbl->CreateTexture(r->dev, 1, 1, 1, 0, D3DFMT_LIN_A8R8G8B8,
                                             D3DPOOL_MANAGED, &r->white)) || !r->white) {
        r->white = NULL;
        return NULL;
    }
    if (SUCCEEDED(r->white->lpVtbl->LockRect(r->white, 0, &lr, NULL, 0))) {
        memset(lr.pBits, 0xFF, 4);
        r->white->lpVtbl->UnlockRect(r->white, 0);
    }
    return r->white;
}

static void replay_texture(Replay *r, const D3D8CapChunk *c)
{
    const D3D8CapTexture *t = c->data;
    const D3D8CapTextureLevel *levels;
    const uint8_t *texels;
    IDirect3DTexture8 *host = NULL;
    size_t total = 0, offset = 0;
    uint32_t l;

    if (c->bytes < sizeof *t || t->id == 0 || t->id >= REPLAY_MAX_TEXTURES)
        return;
    levels = d3d8cap_tail(c, sizeof *t, (size_t)t->levels * sizeof *levels);
    if (!levels)
        return;
    for (l = 0; l < t->levels; l++)
        total += levels[l].bytes;
    texels = d3d8cap_tail(c, sizeof *t + (size_t)t->levels * sizeof *levels, total);
    if (!texels)
        return;

    if (FAILED(r->dev->lpVtbl->CreateTexture(r->dev, t->width, t->height, t->levels,
                                             0, (D3DFORMAT)t->format,
                                             D3DPOOL_MANAGED, &host)) || !host) {
        note("[replay] texture %u (format 0x%02X %ux%u) could not be created\n",
             t->id, t->format, t->width, t->height);
        return;
    }

    /* Level by level, exactly as hle_d3d8_texture.c's upload() does it: the
     * capture holds the guest's own pitch, so a linear texture whose rows are
     * padded to 64 bytes is copied row by row and everything else in one go. */
    for (l = 0; l < t->levels; l++) {
        D3DLOCKED_RECT lr;
        const uint8_t *src = texels + offset;

        offset += levels[l].bytes;
        if (FAILED(host->lpVtbl->LockRect(host, l, &lr, NULL, 0)))
            break;
        if (lr.Pitch == (INT)levels[l].pitch) {
            memcpy(lr.pBits, src, levels[l].bytes);
        } else {
            uint32_t y;
            uint32_t row = levels[l].pitch < (uint32_t)lr.Pitch
                         ? levels[l].pitch : (uint32_t)lr.Pitch;
            for (y = 0; y < levels[l].rows; y++)
                memcpy((uint8_t *)lr.pBits + (size_t)y * (size_t)lr.Pitch,
                       src + (size_t)y * levels[l].pitch, row);
        }
        host->lpVtbl->UnlockRect(host, l);
    }

    if (r->textures[t->id])
        r->textures[t->id]->lpVtbl->Release(r->textures[t->id]);
    r->textures[t->id] = host;
}

static void replay_vs_program(Replay *r, const D3D8CapChunk *c)
{
    const D3D8CapVsProgram *p = c->data;
    const uint32_t *code;
    DWORD host = 0;
    int i;

    if (c->bytes < sizeof *p)
        return;
    code = d3d8cap_tail(c, sizeof *p, (size_t)p->insn_count * 4u * sizeof(uint32_t));
    if (!code)
        return;
    if (FAILED(d3d8_vsh_create_shader((const DWORD *)code, (int)p->insn_count, &host)))
        return;
    /* Each loop walks the capture again and recreates every program; the
     * host has only NV2A_VS_MAX_SLOTS of them, so the previous one goes. */
    for (i = 0; i < r->program_count; i++)
        if (r->programs[i].guest == p->guest_handle) {
            d3d8_vsh_delete_shader(r->programs[i].host);
            r->programs[i].host = host;
            return;
        }
    if (r->program_count < (int)(sizeof r->programs / sizeof r->programs[0])) {
        r->programs[r->program_count].guest = p->guest_handle;
        r->programs[r->program_count].host  = host;
        r->program_count++;
    }
}

static void replay_vs_select(Replay *r, const D3D8CapChunk *c)
{
    const D3D8CapVsSelect *s = c->data;
    int i;

    if (c->bytes < sizeof *s)
        return;
    /* Bit 0 set means the handle is the address of the title's shader object;
     * anything else is an FVF code, which the host takes as it is
     * (hle_d3d8.c, shadow_select_vertex_shader). */
    if (!(s->guest_handle & 1u)) {
        r->dev->lpVtbl->SetVertexShader(r->dev, s->guest_handle);
        return;
    }
    for (i = 0; i < r->program_count; i++)
        if (r->programs[i].guest == s->guest_handle) {
            r->dev->lpVtbl->SetVertexShader(r->dev, r->programs[i].host);
            return;
        }
    /* A program selected but never created inside the captured frame: its
     * CreateVertexShader happened earlier and the capture's frame-start
     * snapshot could only record the handle, not the microcode. */
    r->skipped++;
}

static void replay_draw_up(Replay *r, const D3D8CapChunk *c)
{
    const D3D8CapDrawUp *d = c->data;
    const void *verts;
    D3DPRIMITIVETYPE pt;
    UINT prims;
    uint8_t *loop = NULL;

    if (c->bytes < sizeof *d)
        return;
    verts = d3d8cap_tail(c, sizeof *d, d->vertex_bytes);
    if (!verts)
        return;
    if (!xbox_primitive_to_host(d->prim, d->vertex_count, &pt, &prims)) {
        r->skipped++;
        return;
    }
    if (d->prim == XPT_LINELOOP) {       /* close the loop: repeat vertex 0 */
        loop = malloc((size_t)(d->vertex_count + 1) * d->stride);
        if (!loop) {
            r->skipped++;
            return;
        }
        memcpy(loop, verts, (size_t)d->vertex_count * d->stride);
        memcpy(loop + (size_t)d->vertex_count * d->stride, verts, d->stride);
        verts = loop;
    }
    if (SUCCEEDED(r->dev->lpVtbl->DrawPrimitiveUP(r->dev, pt, prims, verts, d->stride)))
        r->draws++;
    else
        r->skipped++;
    free(loop);
}

/* The host's indexed UP draw converts no primitive types, so fans, polygons,
 * quad lists and line loops are rewritten into index lists here -- the same
 * rewriting hle_d3d8.c does on the live path. */
static void replay_draw_indexed_up(Replay *r, const D3D8CapChunk *c)
{
    const D3D8CapDrawIndexedUp *d = c->data;
    const uint16_t *idx;
    const void *verts;
    uint16_t *list = NULL;
    D3DPRIMITIVETYPE pt;
    UINT prims, vertices = 0, i, n = 0;
    HRESULT hr;

    if (c->bytes < sizeof *d)
        return;
    idx = d3d8cap_tail(c, sizeof *d, d->index_bytes);
    verts = d3d8cap_tail(c, sizeof *d + d->index_bytes, d->vertex_bytes);
    if (!idx || !verts)
        return;
    if (!xbox_primitive_to_host(d->prim, d->index_count, &pt, &prims)) {
        r->skipped++;
        return;
    }
    for (i = 0; i < d->index_count; i++)
        if ((UINT)idx[i] + 1 > vertices)
            vertices = (UINT)idx[i] + 1;

    switch (d->prim) {
    case XPT_TRIANGLEFAN:
    case XPT_POLYGON:
        list = malloc((size_t)prims * 3 * sizeof *list);
        for (i = 0; list && i < prims; i++) {
            list[n++] = idx[0];
            list[n++] = idx[i + 1];
            list[n++] = idx[i + 2];
        }
        pt = D3DPT_TRIANGLELIST;
        break;
    case XPT_QUADLIST:
        list = malloc((size_t)prims * 6 * sizeof *list);
        for (i = 0; list && i < prims; i++) {
            const uint16_t *q = idx + i * 4;
            list[n++] = q[0]; list[n++] = q[1]; list[n++] = q[2];
            list[n++] = q[0]; list[n++] = q[2]; list[n++] = q[3];
        }
        pt = D3DPT_TRIANGLELIST;
        prims *= 2;
        break;
    case XPT_LINELOOP:
        list = malloc(((size_t)d->index_count + 1) * sizeof *list);
        if (list) {
            memcpy(list, idx, (size_t)d->index_count * sizeof *list);
            list[d->index_count] = idx[0];
        }
        break;
    default:
        break;
    }
    if ((d->prim == XPT_TRIANGLEFAN || d->prim == XPT_POLYGON ||
         d->prim == XPT_QUADLIST || d->prim == XPT_LINELOOP) && !list) {
        r->skipped++;
        return;
    }
    hr = r->dev->lpVtbl->DrawIndexedPrimitiveUP(r->dev, pt, 0, vertices, prims,
                                                list ? list : idx, D3DFMT_INDEX16,
                                                verts, d->stride);
    free(list);
    if (SUCCEEDED(hr))
        r->draws++;
    else
        r->skipped++;
}

static void replay_chunk(Replay *r, const D3D8CapChunk *c)
{
    switch (c->type) {
    case D3D8CAP_CLEAR: {
        const D3D8CapClear *p = c->data;
        float z;

        if (c->bytes < sizeof *p)
            break;
        memcpy(&z, &p->z_bits, sizeof z);
        r->dev->lpVtbl->Clear(r->dev, 0, NULL, xbox_clear_flags_to_host(p->flags),
                              p->color, z, p->stencil);
        break;
    }
    case D3D8CAP_TRANSFORM: {
        const D3D8CapTransform *p = c->data;
        D3DMATRIX m;
        DWORD host;

        if (c->bytes < sizeof *p || !xbox_transform_state_to_host(p->state, &host))
            break;
        memcpy(&m, p->m, sizeof m);
        r->dev->lpVtbl->SetTransform(r->dev, (D3DTRANSFORMSTATETYPE)host, &m);
        break;
    }
    case D3D8CAP_VIEWPORT: {
        const D3D8CapViewport *p = c->data;
        D3DVIEWPORT8 vp;

        if (c->bytes < sizeof *p)
            break;
        vp.X = p->x;
        vp.Y = p->y;
        vp.Width = p->width;
        vp.Height = p->height;
        vp.MinZ = p->min_z;
        vp.MaxZ = p->max_z;
        r->dev->lpVtbl->SetViewport(r->dev, &vp);
        break;
    }
    case D3D8CAP_RENDER_STATE: {
        const D3D8CapStateBatch *b = c->data;
        const D3D8CapStatePair *pairs;
        uint32_t i;

        if (c->bytes < sizeof *b)
            break;
        pairs = d3d8cap_tail(c, sizeof *b, (size_t)b->count * sizeof *pairs);
        for (i = 0; pairs && i < b->count; i++)
            r->dev->lpVtbl->SetRenderState(r->dev,
                (D3DRENDERSTATETYPE)pairs[i].state, pairs[i].value);
        break;
    }
    case D3D8CAP_TEXTURE_STAGE_STATE: {
        const D3D8CapStageBatch *b = c->data;
        const D3D8CapStatePair *pairs;
        uint32_t i;

        if (c->bytes < sizeof *b)
            break;
        pairs = d3d8cap_tail(c, sizeof *b, (size_t)b->count * sizeof *pairs);
        for (i = 0; pairs && i < b->count; i++)
            r->dev->lpVtbl->SetTextureStageState(r->dev, b->stage,
                (D3DTEXTURESTAGESTATETYPE)pairs[i].state, pairs[i].value);
        break;
    }
    case D3D8CAP_VS_PROGRAM:
        replay_vs_program(r, c);
        break;
    case D3D8CAP_VS_SELECT:
        replay_vs_select(r, c);
        break;
    case D3D8CAP_VS_CONSTANTS: {
        const D3D8CapVsConstants *p = c->data;
        const float *data;

        if (c->bytes < sizeof *p)
            break;
        data = d3d8cap_tail(c, sizeof *p, (size_t)p->count * 4u * sizeof(float));
        if (data)
            r->dev->lpVtbl->SetVertexShaderConstant(r->dev, (INT)p->first_reg,
                                                    data, p->count);
        break;
    }
    case D3D8CAP_TEXTURE:
        replay_texture(r, c);
        break;
    case D3D8CAP_SET_TEXTURE: {
        const D3D8CapSetTexture *p = c->data;
        IDirect3DTexture8 *tex;

        if (c->bytes < sizeof *p)
            break;
        tex = p->texture_id && p->texture_id < REPLAY_MAX_TEXTURES
            ? r->textures[p->texture_id] : NULL;
        r->dev->lpVtbl->SetTexture(r->dev, p->stage,
            (IDirect3DBaseTexture8 *)(tex ? tex : white_texture(r)));
        break;
    }
    case D3D8CAP_DRAW_UP:
        replay_draw_up(r, c);
        break;
    case D3D8CAP_DRAW_INDEXED_UP:
        replay_draw_indexed_up(r, c);
        break;
    default:
        /* An unknown chunk from a newer writer cannot appear: the reader
         * refuses a version it does not know. Anything else is skipped so a
         * future additive chunk does not stop an old replay. */
        break;
    }
}

static void usage(void)
{
    fprintf(stderr,
        "usage: d3d8_replay <capture%s> [--out <prefix>] [--loops <n>]\n"
        "                   [--dump-every] [--hold] [--quiet]\n",
        D3D8CAP_EXTENSION);
}

int main(int argc, char **argv)
{
    const char *path = NULL, *prefix = "replay";
    int loops = 1, dump_every = 0, hold = 0, i, loop;
    char err[128], out[512];
    D3D8CapReader *cap;
    const D3D8CapHeader *h;
    Replay r;
    D3DPRESENT_PARAMETERS pp;
    IDirect3D8 *d3d;
    HWND hwnd;
    HRESULT hr;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--out") && i + 1 < argc)
            prefix = argv[++i];
        else if (!strcmp(argv[i], "--loops") && i + 1 < argc)
            loops = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--dump-every"))
            dump_every = 1;
        else if (!strcmp(argv[i], "--hold"))
            hold = 1;
        else if (!strcmp(argv[i], "--quiet"))
            g_quiet = 1;
        else if (argv[i][0] == '-') {
            usage();
            return 2;
        } else {
            path = argv[i];
        }
    }
    if (!path || loops < 1) {
        usage();
        return 2;
    }

    cap = d3d8cap_open(path, err, sizeof err);
    if (!cap) {
        fprintf(stderr, "[replay] %s: %s\n", path, err);
        return 1;
    }
    h = d3d8cap_header(cap);
    note("[replay] %s: frame %u, %ux%u, %u chunks\n",
         path, h->frame, h->width, h->height, h->chunk_count);

    memset(&r, 0, sizeof r);
    r.width  = h->width  ? h->width  : 640;
    r.height = h->height ? h->height : 480;

    hwnd = replay_window(r.width, r.height);
    if (!hwnd) {
        fprintf(stderr, "[replay] CreateWindow failed (%lu)\n", GetLastError());
        d3d8cap_close_read(cap);
        return 1;
    }

    memset(&pp, 0, sizeof pp);
    pp.BackBufferWidth = r.width;
    pp.BackBufferHeight = r.height;
    pp.BackBufferCount = 1;
    pp.hDeviceWindow = hwnd;
    pp.Windowed = TRUE;
    pp.EnableAutoDepthStencil = TRUE;

    d3d = xbox_Direct3DCreate8(0);
    hr = d3d ? d3d->lpVtbl->CreateDevice(d3d, 0, 1 /* HAL */, hwnd, 0, &pp, &r.dev)
             : E_FAIL;
    if (FAILED(hr) || !r.dev) {
        fprintf(stderr, "[replay] CreateDevice failed (0x%08lX)\n", (unsigned long)hr);
        d3d8cap_close_read(cap);
        return 1;
    }
    xbox_D3D8SetPresentInterval(0);      /* never wait for vblank: this is a tool */

    /* The same starting point shadow mode gives its device, so a replay and a
     * live frame begin from the same state. Everything else in the capture is
     * the title's own. */
    r.dev->lpVtbl->SetRenderState(r.dev, D3DRS_CULLMODE, D3DCULL_NONE);
    r.dev->lpVtbl->SetRenderState(r.dev, D3DRS_LIGHTING, FALSE);
    r.dev->lpVtbl->SetTexture(r.dev, 0, NULL);

    for (loop = 0; loop < loops; loop++) {
        D3D8CapChunk c;

        r.draws = r.skipped = 0;
        d3d8cap_rewind(cap);
        while (d3d8cap_next(cap, &c))
            replay_chunk(&r, &c);

        if (dump_every || loop == loops - 1) {
            snprintf(out, sizeof out, "%s%03d.bmp", prefix, loop);
            dump_bmp(r.dev, out, r.width, r.height);
        }
        r.dev->lpVtbl->Swap(r.dev, 0);
        pump();
        note("[replay] loop %d: %lu draws, %lu skipped\n", loop, r.draws, r.skipped);
    }

    if (hold) {
        MSG msg;
        note("[replay] holding the window open; close it to exit\n");
        while (GetMessageA(&msg, NULL, 0, 0) > 0) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
    }

    for (i = 1; i < REPLAY_MAX_TEXTURES; i++)
        if (r.textures[i])
            r.textures[i]->lpVtbl->Release(r.textures[i]);
    if (r.white)
        r.white->lpVtbl->Release(r.white);
    d3d8cap_close_read(cap);
    return 0;
}
