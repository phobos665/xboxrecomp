/*
 * hle_d3d8.c -- Direct3D 8 functions replaced by name.
 *
 * The route the DOAXBV port proved: replace D3D8 at its API and translate to
 * the host's graphics API, rather than emulate the NV2A beneath it. Measured
 * for Burnout 2 by tools/hle_audit/device_refs.py: no game code reads the
 * D3D device global itself -- 118 references, all inside the D3D section --
 * but two game functions fill their own push buffers through BeginPush
 * (call sites 0x000FA0D9 and 0x000FBE46), and those bypass any replacement
 * made here.
 *
 * The display-filter setters come first because their correct behaviour on a
 * PC is to do nothing: they tune the TV encoder's flicker filter, which a
 * monitor does not have.
 *
 * Shadow mode, RECOMP_HLE_D3D8=shadow
 *
 * The step toward drawing through the host renderer. Every replacement below
 * runs the title's own lifted body first (HLE_ORIGINAL), so the guest device,
 * the push buffer and every frame the executor draws stay exactly as they
 * were. Then, with the switch set, the same call is repeated on a host D3D11
 * device in a window of its own. Registers and return values are the
 * original's, so the title sees no difference. Without the switch these
 * replacements only run the originals.
 *
 * What reaches the host device so far:
 *   - CreateDevice (size), Clear, Swap;
 *   - vertex shaders: FVF codes as they are, and programs the title created,
 *     replayed from their microcode;
 *   - SetTransform, SetViewport;
 *   - DrawVerticesUP and DrawIndexedVerticesUP, the draws whose vertices the
 *     title hands over directly. In a profiled first minute of Burnout 2
 *     these were 29,087 of 36,275 draws; the rest go through vertex buffers;
 *   - SetTexture, in hle_d3d8_texture.c;
 *   - render and texture stage states, in hle_d3d8_state.c;
 *   - SetStreamSource, DrawVertices and DrawIndexedVertices, the vertex
 *     buffer draws, and the vertex shader constants, in hle_d3d8_vertex.c.
 * Draws under a vertex program use the declaration the XDK parsed into the
 * shader object (shadow_read_declaration), with NORMPACKED3 normals unpacked
 * on the CPU and the XDK's screen-space transform undone on the host (see
 * "viewports"). A program whose declaration the host cannot lay out is
 * counted and skipped, and so are draws under a declaration-only shader.
 *
 * Every call into the host renderer goes through the wrappers in
 * hle_d3d8_record.c (host_*), which is also where a frame capture is
 * recorded (RECOMP_D3D8_CAPTURE); see hle_d3d8_record.h.
 *
 * RECOMP_HLE_D3D8_DUMP=<prefix> writes the host frame to <prefix>NNN.bmp
 * every RECOMP_HLE_D3D8_DUMP_EVERY swaps (default 300), at most 24 files --
 * the same format as the executor's RECOMP_FB_DUMP, to put them side by side.
 *
 * Two host facts shape it:
 *   - Guest threads are real host threads. DXGI's Present sends messages to
 *     the window's thread and waits for them, so the window gets a thread of
 *     its own that does nothing but pump it.
 *   - The host D3D8 device is one process-wide object. The host FMV player
 *     (RECOMP_FMV_HOST) creates it too, so the two switches exclude each
 *     other.
 *
 * Each logs its first call, so a run shows the replacement was reached.
 */
#include "platform/xbox_winnt.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hle.h"

#ifdef _WIN32
#include "d3d8_xbox.h"
#include "d3d8_vsh.h"
#include "d3d8_xbox_map.h"
#include "hle_d3d8_record.h"
#endif

static void first_call(int *seen, const char *name, uint32_t arg)
{
    if (!*seen) {
        *seen = 1;
        fprintf(stderr, "[HLE] %s(0x%X) replaced by name\n", name, arg);
        fflush(stderr);
    }
}

/* void D3DDevice_SetFlickerFilter(DWORD Filter) -- TV encoder only. */
HLE_EXPORT(D3DDevice_SetFlickerFilter)
{
    static int seen;
    first_call(&seen, "D3DDevice_SetFlickerFilter", HLE_ARG(0));
    HLE_RETURN(0);
}

/* void D3DDevice_SetSoftDisplayFilter(BOOL Enable) -- TV encoder only. */
HLE_EXPORT(D3DDevice_SetSoftDisplayFilter)
{
    static int seen;
    first_call(&seen, "D3DDevice_SetSoftDisplayFilter", HLE_ARG(0));
    HLE_RETURN(0);
}

/* ------------------------------------------------------------------ shadow */

#ifdef _WIN32

#define SHADOW_WM_DESTROY (WM_APP + 1)

static int                g_shadow_mode = -1;
static int                g_shadow_tried;
static IDirect3DDevice8  *g_shadow;
static UINT               g_shadow_width, g_shadow_height;
/* The render target being drawn into: the back buffer, or an offscreen one the
 * title selected (shadow_set_render_target). Viewports and the programs'
 * screen-space undo are relative to it. */
static UINT               g_target_width, g_target_height;
static unsigned long      g_target_sets, g_target_scratch, g_target_failed;
static unsigned long      g_frame_draws;    /* draws since the last Swap */
static DWORD              g_shadow_create_thread;
static DWORD              g_shadow_swap_thread;
static int                g_shadow_thread_notes;
static unsigned long      g_shadow_clears;
static unsigned long      g_shadow_swaps;
static uint32_t           g_shadow_last_color;
static DWORD              g_shadow_last_report;

static int shadow_requested(void)
{
    if (g_shadow_mode < 0) {
        const char *mode = getenv("RECOMP_HLE_D3D8");
        const char *fmv  = getenv("RECOMP_FMV_HOST");

        g_shadow_mode = mode && strcmp(mode, "shadow") == 0;
        if (g_shadow_mode && fmv && *fmv && strcmp(fmv, "0") != 0) {
            fprintf(stderr, "[HLE-D3D8] shadow mode off: RECOMP_FMV_HOST creates the "
                    "same host device, and there is only one\n");
            g_shadow_mode = 0;
        }
    }
    return g_shadow_mode;
}

static LRESULT CALLBACK shadow_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CLOSE:                       /* closing it must not end the title */
        ShowWindow(hwnd, SW_HIDE);
        return 0;
    case SHADOW_WM_DESTROY:              /* DestroyWindow only works on this thread */
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

typedef struct {
    UINT   width, height;
    HWND   hwnd;
    HANDLE ready;
} shadow_window_request;

/* Owns the shadow window for the life of the process and pumps it, so a
 * Present from any guest thread always has a thread answering its messages. */
static DWORD WINAPI shadow_window_thread(LPVOID param)
{
    shadow_window_request *req = param;
    WNDCLASSA wc;
    RECT r;
    MSG msg;

    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc   = shadow_wndproc;
    wc.hInstance     = GetModuleHandleA(NULL);
    wc.hCursor       = LoadCursorA(NULL, MAKEINTRESOURCEA(32512));   /* IDC_ARROW */
    wc.lpszClassName = "xboxrecomp_hle_d3d8_shadow";
    RegisterClassA(&wc);

    r.left = 0;
    r.top = 0;
    r.right = (LONG)req->width;
    r.bottom = (LONG)req->height;
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    req->hwnd = CreateWindowA(wc.lpszClassName, "xboxrecomp - D3D8 replacement (shadow)",
                              WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                              CW_USEDEFAULT, CW_USEDEFAULT,
                              r.right - r.left, r.bottom - r.top,
                              NULL, NULL, wc.hInstance, NULL);
    if (!req->hwnd)
        fprintf(stderr, "[HLE-D3D8] shadow: CreateWindow failed (%lu)\n", GetLastError());
    SetEvent(req->ready);                /* req lives on the waiting caller's stack */
    if (!req->hwnd)
        return 1;

    while (GetMessageA(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    return 0;
}

static HWND shadow_window(UINT width, UINT height)
{
    shadow_window_request req;
    HANDLE thread;

    memset(&req, 0, sizeof req);
    req.width = width;
    req.height = height;
    req.ready = CreateEventA(NULL, TRUE, FALSE, NULL);
    if (!req.ready)
        return NULL;
    thread = CreateThread(NULL, 0, shadow_window_thread, &req, 0, NULL);
    if (!thread) {
        fprintf(stderr, "[HLE-D3D8] shadow: CreateThread failed (%lu)\n", GetLastError());
        CloseHandle(req.ready);
        return NULL;
    }
    WaitForSingleObject(req.ready, INFINITE);
    CloseHandle(req.ready);
    CloseHandle(thread);                 /* it keeps running; only the handle goes */
    return req.hwnd;
}

/* The surfaces the XDK sets as target and depth inside CreateDevice -- the
 * frame buffer and the automatic depth buffer, 0 until seen -- and the host
 * device's own depth surface (shadow_set_render_target). */
static int      g_in_create_device;
static uint32_t g_backbuffer_va, g_autodepth_va;
static IDirect3DSurface8 *g_device_depth;

/* ------------------------------------------------------------- viewports */

/* Xbox vertex programs end in the XDK's screen-space transform: oPos comes
 * out in render-target pixels, through the viewport scale and offset the XDK
 * keeps in constants 58 and 59 (c-38 and c-37), with Z scaled to the depth
 * buffer's range. The values follow Cxbx-Reloaded
 * (GetXboxViewportOffsetAndScale, GetZScaleForPixelContainer). The host
 * programs undo a whole render target's worth of it
 * (d3d8_vsh_set_screenspace), which leaves the viewport already applied:
 * program draws use a host viewport over the whole target with depth 0..1,
 * fixed-function draws the title's own.
 *
 * Z is scaled for the format of the depth surface bound with the current
 * render target (shadow_set_render_target), the CreateDevice one to begin
 * with.
 *
 * Not handled: X_D3DSCM_NORESERVEDCONSTANTS, which frees 58 and 59 for the
 * title (a title that then sets them wins until its next SetViewport). */
static float        g_z_scale = 1.0f;
static D3DVIEWPORT8 g_title_viewport;
static int          g_title_viewport_set;
static int          g_host_viewport_mode = -1;   /* 0 the title's, 1 whole target */

static float xbox_depth_z_scale(uint32_t format)
{
    switch (format) {
    case 0x2C: case 0x30: return 65535.0f;       /* D16, LIN_D16 */
    case 0x2A: case 0x2E: return 16777215.0f;    /* D24S8, LIN_D24S8 */
    case 0x2D: case 0x31: return 511.9375f;      /* F16, LIN_F16 */
    case 0x2B: case 0x2F: return 1.0e30f;        /* F24S8, LIN_F24S8 */
    default:              return 1.0f;
    }
}

static void shadow_viewport_constants(const D3DVIEWPORT8 *vp)
{
    float half_w = (float)g_target_width / 2.0f;
    float half_h = (float)g_target_height / 2.0f;
    float reserved[8] = {
        (float)vp->Width / 2.0f, -(float)vp->Height / 2.0f,
        (vp->MaxZ - vp->MinZ) * g_z_scale, 1.0f,
        (float)vp->Width / 2.0f + (float)vp->X, (float)vp->Height / 2.0f + (float)vp->Y,
        vp->MinZ * g_z_scale, 0.0f
    };
    float scale[4]  = { half_w, -half_h, g_z_scale, 1.0f };
    float offset[4] = { half_w, half_h, 0.0f, 0.0f };

    host_vsh_set_constant(58, reserved, 2);
    host_vsh_set_screenspace(scale, offset);
}

static void shadow_use_viewport(int whole_target)
{
    D3DVIEWPORT8 vp;

    if (g_host_viewport_mode == whole_target)
        return;
    g_host_viewport_mode = whole_target;
    if (!whole_target && g_title_viewport_set) {
        vp = g_title_viewport;
    } else {
        vp.X = 0;
        vp.Y = 0;
        vp.Width = g_target_width;
        vp.Height = g_target_height;
        vp.MinZ = 0.0f;
        vp.MaxZ = 1.0f;
    }
    host_SetViewport(g_shadow, &vp);
}

/* The guest's D3DPRESENT_PARAMETERS is the Xbox layout in 32-bit guest
 * memory: BackBufferWidth at +0, BackBufferHeight at +4, a guest HWND at +24.
 * The host struct holds a pointer-sized HWND, so only the fields the host
 * device needs are copied, one by one. One attempt per process: a title that
 * recreates its device must not leave a window behind for each failure. */
static void shadow_create(uint32_t pp_va)
{
    D3DPRESENT_PARAMETERS pp;
    IDirect3D8 *d3d;
    HRESULT hr;
    UINT width = 640, height = 480;
    HWND hwnd;

    g_shadow_tried = 1;
    if (pp_va && HLE_MEM32(pp_va + 0) && HLE_MEM32(pp_va + 4)) {
        width  = HLE_MEM32(pp_va + 0);
        height = HLE_MEM32(pp_va + 4);
    }
    /* EnableAutoDepthStencil at +32, AutoDepthStencilFormat at +36. */
    if (pp_va && HLE_MEM32(pp_va + 32))
        g_z_scale = xbox_depth_z_scale(HLE_MEM32(pp_va + 36));

    hwnd = shadow_window(width, height);
    if (!hwnd)
        return;

    memset(&pp, 0, sizeof pp);
    pp.BackBufferWidth = width;
    pp.BackBufferHeight = height;
    pp.BackBufferCount = 1;
    pp.hDeviceWindow = hwnd;
    pp.Windowed = TRUE;
    pp.EnableAutoDepthStencil = TRUE;

    d3d = xbox_Direct3DCreate8(0);
    hr = d3d ? d3d->lpVtbl->CreateDevice(d3d, 0, 1 /* HAL */, hwnd, 0, &pp, &g_shadow)
             : E_FAIL;
    if (FAILED(hr) || !g_shadow) {
        fprintf(stderr, "[HLE-D3D8] shadow: CreateDevice failed (0x%08lX)\n", (unsigned long)hr);
        g_shadow = NULL;
        PostMessageA(hwnd, SHADOW_WM_DESTROY, 0, 0);
        return;
    }
    /* Never wait for vertical blank here. The title paces its loader on the
     * frames it presents itself; a vsync wait on this side device throttled
     * Burnout 2's whole loop to 27 frames a second. */
    xbox_D3D8SetPresentInterval(0);
    g_shadow_width = width;
    g_shadow_height = height;
    g_target_width = width;
    g_target_height = height;
    g_device_depth = host_DeviceDepthSurface(g_shadow);
    {
        /* Until the title sets one: the whole back buffer. */
        D3DVIEWPORT8 whole = { 0, 0, width, height, 0.0f, 1.0f };
        shadow_viewport_constants(&whole);
    }

    /* Until the first draw applies the title's own states: nothing culled and
     * no texture. Lighting stays off for good (hle_d3d8_state.c): lights and
     * the material are not forwarded, and an unlit vertex keeps its colour. */
    host_SetRenderState(g_shadow, D3DRS_CULLMODE, D3DCULL_NONE);
    host_SetRenderState(g_shadow, D3DRS_LIGHTING, FALSE);
    host_SetTexture(g_shadow, 0, NULL);

    g_shadow_create_thread = GetCurrentThreadId();
    fprintf(stderr, "[HLE-D3D8] shadow device %ux%u, from the title's own "
            "CreateDevice parameters, on guest thread %lu; vertex program Z "
            "scale %g\n", width, height, (unsigned long)g_shadow_create_thread,
            g_z_scale);
    fflush(stderr);
}

/* xbox_clear_flags_to_host() is in d3d8_xbox_map.h with the other Xbox-to-host
 * value tables. The first Clear calls log the raw flags below, so the mapping
 * stays checkable against what the title really passes. */

/* ----------------------------------------------------------- vertex shaders */

/* Xbox SetVertexShader takes either an FVF code (bit 0 clear) or the address
 * of the title's X_D3DVertexShader with bit 0 set (Cxbx-Reloaded,
 * XbVertexShader.h, VshHandleIsVertexShader). FVF bits are the PC's, so an
 * FVF code goes to the host as it is. A program is created on the host when
 * the title creates it, and found again here by its guest handle. */
#define SHADOW_MAX_PROGRAMS 128

enum { SHADER_DECLARATION, SHADER_HOST_PROGRAM, SHADER_NOT_REPLAYED };

#define SHADOW_MAX_PACKED 4              /* NORMPACKED3 registers per program */

struct shadow_program {
    uint32_t guest;
    DWORD    host;
    int      kind;
    /* From the declaration (shadow_read_declaration), host programs only. */
    int      has_declaration;            /* the host has its vertex layout */
    UINT     extent;                     /* bytes of a vertex it reads */
    int      packed_count;               /* NORMPACKED3 registers */
    UINT     packed_offset[SHADOW_MAX_PACKED];
};

static struct shadow_program g_programs[SHADOW_MAX_PROGRAMS];
static int      g_program_count;
static uint32_t g_shadow_vs;             /* guest handle last selected */
static int      g_shadow_vs_is_program;  /* bit 0 set: a shader object */
static int      g_shadow_vs_kind;        /* its kind, or -1 if never created */
static int      g_shadow_vs_slot = -1;   /* its g_programs entry, or -1 */

/* The handle is the address of the title's shader object, so a shader
 * created after another was deleted can reuse its handle. Creation replaces
 * an entry, so each handle has at most one. */
static int shadow_program_find(uint32_t guest)
{
    int i;

    for (i = 0; i < g_program_count; i++)
        if (g_programs[i].guest == guest)
            return i;
    return -1;
}

static void shadow_select_vertex_shader(uint32_t handle)
{
    int i;

    g_shadow_vs = handle;
    g_shadow_vs_is_program = (handle & 1) != 0;
    if (!g_shadow_vs_is_program) {
        g_shadow_vs_kind = -1;
        g_shadow_vs_slot = -1;
        host_SetVertexShader(g_shadow, handle);
        return;
    }
    i = shadow_program_find(handle);
    g_shadow_vs_slot = i;
    g_shadow_vs_kind = i >= 0 ? g_programs[i].kind : -1;
    if (g_shadow_vs_kind == SHADER_HOST_PROGRAM)
        host_SetVertexShader(g_shadow, g_programs[i].host);
}

/* ---------------------------------------------------------------- drawing */

/* The Xbox primitive types (XPT_*) and xbox_primitive_to_host() are in
 * d3d8_xbox_map.h. */

/* FVF vertex size, as the host computes it for its input layout. A draw
 * whose stride disagrees would be read at the wrong offsets, so it is
 * skipped and counted rather than drawn as noise. */
static UINT fvf_stride(DWORD fvf)
{
    UINT size = 0, tex;

    switch (fvf & 0x00E) {
    case 0x002: size = 12; break;        /* XYZ */
    case 0x004: size = 16; break;        /* XYZRHW */
    case 0x006: size = 16; break;        /* XYZB1 */
    case 0x008: size = 20; break;
    case 0x00A: size = 24; break;
    case 0x00C: size = 28; break;
    }
    if (fvf & 0x010) size += 12;         /* NORMAL */
    if (fvf & 0x040) size += 4;          /* DIFFUSE */
    if (fvf & 0x080) size += 4;          /* SPECULAR */
    tex = (fvf >> 8) & 0xF;
    if (tex > 8)                         /* TEX8 is the most; more is garbage */
        tex = 8;
    for (UINT i = 0; i < tex; i++) {
        /* D3DFVF_TEXCOORDSIZEn: 2 bits per set from bit 16; 0 means 2D. */
        static const UINT dims[4] = { 2, 3, 4, 1 };
        size += 4 * dims[(fvf >> (16 + 2 * i)) & 3];
    }
    return size;
}

/* hle_d3d8_state.c */
void hle_d3d8_pixel_shader_selected(uint32_t handle);
void hle_d3d8_shadow_apply_states(IDirect3DDevice8 *dev);

static unsigned long g_draws_up, g_draws_indexed_up, g_draws_vb, g_draws_indexed_vb,
                     g_draws_program, g_draws_declaration, g_draws_unknown_vs,
                     g_draws_stride, g_draws_primitive, g_draws_failed;
/* Draws arriving on a guest thread other than the one that swaps. A loader
 * thread drawing to warm caches puts geometry through the host that no
 * presented frame ever contains -- it would count as drawn and never show. */
static unsigned long g_draws_off_thread;

/* The vertex formats draws arrive with, whether or not they are then drawn:
 * a shader handle seen for the first time is logged, up to a limit. */
static void note_draw_format(uint32_t xpt, uint32_t stride)
{
    static uint32_t seen[24];
    static int nseen;
    int i;

    for (i = 0; i < nseen; i++)
        if (seen[i] == g_shadow_vs)
            return;
    if (nseen < (int)(sizeof seen / sizeof seen[0])) {
        seen[nseen++] = g_shadow_vs;
        fprintf(stderr, "[HLE-D3D8] shadow draw: %s 0x%08X, stride %u, primitive %u\n",
                g_shadow_vs_is_program ? "vertex program" : "FVF",
                g_shadow_vs, stride, xpt);
    }
}

/* Common checks for a draw under the current vertex shader. */
static int shadow_can_draw(uint32_t xpt, uint32_t stride)
{
    if (g_shadow_swap_thread && GetCurrentThreadId() != g_shadow_swap_thread)
        g_draws_off_thread++;
    note_draw_format(xpt, stride);
    if (g_shadow_vs_is_program) {
        const struct shadow_program *p;

        if (g_shadow_vs_kind == SHADER_DECLARATION) {
            g_draws_declaration++;       /* fixed function by declaration */
            return 0;
        }
        if (g_shadow_vs_kind != SHADER_HOST_PROGRAM || g_shadow_vs_slot < 0) {
            g_draws_unknown_vs++;
            return 0;
        }
        p = &g_programs[g_shadow_vs_slot];
        if (!p->has_declaration) {
            g_draws_program++;           /* no vertex layout for the host */
            return 0;
        }
        if (stride < p->extent) {        /* the layout would read past a vertex */
            g_draws_stride++;
            return 0;
        }
        shadow_use_viewport(1);
    } else {
        if (fvf_stride(g_shadow_vs) != stride) {
            g_draws_stride++;
            return 0;
        }
        shadow_use_viewport(0);
    }
    /* The title's render and texture stage states as they stand now, read
     * from its own state arrays (hle_d3d8_state.c). */
    hle_d3d8_shadow_apply_states(g_shadow);
    g_frame_draws++;
    return 1;
}

/* ------------------------------------------------------------ frame dumps */

static void shadow_dump_frame(void)
{
    static const char *prefix;
    static int configured, every = 300, written;
    static unsigned long min_draws, last_dump;
    IDirect3DSurface8 *surf = NULL;
    D3DLOCKED_RECT lr;
    char path[512];
    uint8_t hdr[54];
    UINT w, h, y, x, pad;
    uint32_t filesz;
    FILE *f;

    if (!configured) {
        const char *e = getenv("RECOMP_HLE_D3D8_DUMP_EVERY");
        configured = 1;
        prefix = getenv("RECOMP_HLE_D3D8_DUMP");
        if (e && atoi(e) > 0)
            every = atoi(e);
        /* RECOMP_HLE_D3D8_DUMP_MINDRAWS=<n>: dump only frames with at least n
         * draws, at least `every` swaps apart. A title's 3D frames can be rare
         * among its 2D ones, and a fixed interval keeps missing them. */
        e = getenv("RECOMP_HLE_D3D8_DUMP_MINDRAWS");
        if (e && atol(e) > 0)
            min_draws = (unsigned long)atol(e);
    }
    if (!prefix || !*prefix || written >= 24)
        return;
    if (min_draws) {
        if (g_frame_draws < min_draws || (last_dump && g_shadow_swaps - last_dump < (unsigned long)every))
            return;
        last_dump = g_shadow_swaps;
    } else if ((g_shadow_swaps % (unsigned long)every) != 0) {
        return;
    }

    if (FAILED(g_shadow->lpVtbl->GetBackBuffer(g_shadow, 0, 0, &surf)) || !surf)
        return;
    if (FAILED(surf->lpVtbl->LockRect(surf, &lr, NULL, D3DLOCK_READONLY))) {
        surf->lpVtbl->Release(surf);
        return;
    }
    /* The swap chain is R8G8B8A8 (d3d8_device.c) at the size it was created. */
    w = g_shadow_width;
    h = g_shadow_height;
    pad = (4 - ((w * 3) & 3)) & 3;
    filesz = 54 + (w * 3 + pad) * h;

    snprintf(path, sizeof path, "%s%03d.bmp", prefix, written++);
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
        fprintf(stderr, "[HLE-D3D8] shadow frame %lu -> %s\n", g_shadow_swaps, path);
    }
    surf->lpVtbl->UnlockRect(surf);
    surf->lpVtbl->Release(surf);
}

#endif /* _WIN32 */

HLE_ORIGINAL(Direct3D_CreateDevice);
HLE_ORIGINAL(D3DDevice_Clear);
HLE_ORIGINAL(D3DDevice_Swap);
HLE_ORIGINAL(D3DDevice_CreateVertexShader);
HLE_ORIGINAL(D3DDevice_SetVertexShader);
HLE_ORIGINAL(D3DDevice_SelectVertexShader);
HLE_ORIGINAL(D3DDevice_SetTransform);
HLE_ORIGINAL(D3DDevice_SetViewport);
HLE_ORIGINAL(D3DDevice_SetRenderTarget);
HLE_ORIGINAL(D3DDevice_SetPixelShader);
HLE_ORIGINAL(D3DDevice_SetVertexDataColor);
HLE_ORIGINAL(D3DDevice_SetVertexData2f);
HLE_ORIGINAL(D3DDevice_DrawVerticesUP);
HLE_ORIGINAL(D3DDevice_DrawIndexedVerticesUP);

/* tools.recomp keeps the original whenever it replaces one of these names and
 * lifts its body, so a missing one means a build/lift mismatch or a body this
 * lift left out. Returning without running it would turn the call into a
 * silent no-op, so it is reported every time. */
static int original_missing(void (*fn)(void), const char *name)
{
    if (fn)
        return 0;
    fprintf(stderr, "[HLE] %s: original body missing -- regenerate the lift\n", name);
    return 1;
}

/* HRESULT Direct3D_CreateDevice(UINT Adapter, D3DDEVTYPE DeviceType,
 *     HWND hFocusWindow, DWORD BehaviorFlags,
 *     D3DPRESENT_PARAMETERS *pPresentationParameters,
 *     IDirect3DDevice8 **ppReturnedDeviceInterface)                        */
HLE_EXPORT(Direct3D_CreateDevice)
{
    static int seen;
    uint32_t pp_va = HLE_ARG(4);

    first_call(&seen, "Direct3D_CreateDevice", pp_va);
    if (original_missing(hle_original_Direct3D_CreateDevice, "Direct3D_CreateDevice"))
        HLE_RETURN(0x80004005u);                 /* E_FAIL */
#ifdef _WIN32
    g_in_create_device = 1;
#endif
    HLE_CALL_ORIGINAL(Direct3D_CreateDevice);
#ifdef _WIN32
    g_in_create_device = 0;
    if (!g_backbuffer_va)
        fprintf(stderr, "[HLE-D3D8] CreateDevice set no render target; the back "
                "buffer is taken to be any parentless surface of its size\n");
    /* Only beside a guest device that exists: the original's HRESULT. */
    if (shadow_requested() && !g_shadow_tried && (int32_t)g_eax >= 0)
        shadow_create(pp_va);
#endif
}

/* HRESULT D3DDevice_Clear(DWORD Count, const D3DRECT *pRects, DWORD Flags,
 *     D3DCOLOR Color, float Z, DWORD Stencil)                               */
HLE_EXPORT(D3DDevice_Clear)
{
    static int seen;
#ifdef _WIN32
    uint32_t flags = HLE_ARG(2), color = HLE_ARG(3);
    uint32_t z_bits = HLE_ARG(4), stencil = HLE_ARG(5);
#endif

    first_call(&seen, "D3DDevice_Clear", HLE_ARG(2));
    if (original_missing(hle_original_D3DDevice_Clear, "D3DDevice_Clear"))
        HLE_RETURN(0x80004005u);
    HLE_CALL_ORIGINAL(D3DDevice_Clear);
#ifdef _WIN32
    if (g_shadow) {
        float z;

        memcpy(&z, &z_bits, sizeof z);
        if (g_shadow_clears < 4)
            fprintf(stderr, "[HLE-D3D8] shadow Clear: flags 0x%08X color 0x%08X "
                    "z %g stencil %u\n", flags, color, z, stencil);
        host_Clear(g_shadow, 0, NULL, xbox_clear_flags_to_host(flags), color, z, stencil);
        g_shadow_clears++;
        g_shadow_last_color = color;
    }
#endif
}

/* HRESULT D3DDevice_Swap(DWORD Flags)                                       */
HLE_EXPORT(D3DDevice_Swap)
{
    static int seen;

    first_call(&seen, "D3DDevice_Swap", HLE_ARG(0));
    if (original_missing(hle_original_D3DDevice_Swap, "D3DDevice_Swap"))
        HLE_RETURN(0x80004005u);
    HLE_CALL_ORIGINAL(D3DDevice_Swap);
#ifdef _WIN32
    if (g_shadow) {
        DWORD now = GetTickCount();
        DWORD thread = GetCurrentThreadId();

        /* Which guest threads swap, against the one that created the device.
         * Capped, in case they alternate. */
        if (thread != g_shadow_swap_thread && g_shadow_thread_notes < 8) {
            fprintf(stderr, "[HLE-D3D8] shadow: Swap on guest thread %lu (device "
                    "created on %lu)\n", (unsigned long)thread,
                    (unsigned long)g_shadow_create_thread);
            g_shadow_thread_notes++;
        }
        g_shadow_swap_thread = thread;

        g_shadow_swaps++;
        /* The frame boundary for capture: closes the frame being recorded, or
         * starts recording if this is the requested swap. */
        hle_d3d8_capture_swap(g_shadow_swaps, g_shadow_width, g_shadow_height);
        shadow_dump_frame();             /* before Present discards the buffer */
        g_frame_draws = 0;
        host_Swap(g_shadow, 0);
        if (!g_shadow_last_report) {
            g_shadow_last_report = now;
        } else if (now - g_shadow_last_report >= 5000) {
            fprintf(stderr, "[HLE-D3D8] shadow: %lu swaps, %lu clears, last clear "
                    "color 0x%08X | draws: %lu UP + %lu indexed UP + %lu buffer + "
                    "%lu indexed buffer drawn; skipped %lu program without layout, %lu "
                    "declaration shader, %lu unknown shader, %lu stride, %lu "
                    "primitive, %lu failed; %lu off the swapping thread\n",
                    g_shadow_swaps, g_shadow_clears, g_shadow_last_color,
                    g_draws_up, g_draws_indexed_up, g_draws_vb, g_draws_indexed_vb,
                    g_draws_program, g_draws_declaration, g_draws_unknown_vs,
                    g_draws_stride, g_draws_primitive, g_draws_failed,
                    g_draws_off_thread);
            if (g_target_sets)
                fprintf(stderr, "[HLE-D3D8] shadow render targets: %lu set, %lu to a "
                        "scratch target, %lu failed\n", g_target_sets,
                        g_target_scratch, g_target_failed);
            fflush(stderr);
            g_shadow_last_report = now;
        }
    }
#endif
}

#ifdef _WIN32
/* The XDK keeps the declaration it parsed in the shader object itself:
 * X_D3DVertexShader { RefCount, Flags, ProgramSize, ProgramAndConstantsDwords,
 * BYTE Dimensionality[4], X_VERTEXATTRIBUTEFORMAT VertexAttribute } -- 16 slots
 * of { StreamIndex, Offset, Format, 4 bytes }, one per vertex register
 * (Cxbx-Reloaded, XbD3D8Types.h). Logged for the first programs so a run shows
 * which vertex formats a title's programs read before the host is taught
 * them. Format X_D3DVSDT_NONE (0x02) or 0 is an unused register. */
static void note_vertex_attributes(uint32_t handle)
{
    static int notes;
    uint32_t object = handle & ~1u, i;
    char line[640];
    int n;

    if (notes >= 32 || !object)
        return;
    notes++;
    n = snprintf(line, sizeof line, "[HLE-D3D8] shadow declaration 0x%08X flags 0x%X:",
                 handle, HLE_MEM32(object + 4u));
    for (i = 0; i < 16u && n > 0 && n < (int)sizeof line; i++) {
        uint32_t slot = object + 20u + i * 16u;
        uint32_t format = HLE_MEM32(slot + 8u);

        if (format <= 0x02u)
            continue;
        n += snprintf(line + n, sizeof line - (size_t)n, " v%u=s%u+%u:%02X", i,
                      HLE_MEM32(slot), HLE_MEM32(slot + 4u), format);
    }
    fprintf(stderr, "%s\n", line);
}

/* An X_D3DVSDT format the host reads as it is: its DXGI format and size.
 * D3DCOLOR is stored BGRA. Three-component shorts and bytes have no DXGI
 * format, and unnormalised shorts would need an integer shader input, so
 * those return 0. */
static int xbox_vsdt_to_dxgi(uint32_t format, DXGI_FORMAT *dxgi, UINT *size)
{
    switch (format) {
    case 0x12: *dxgi = DXGI_FORMAT_R32_FLOAT;          *size = 4;  return 1; /* FLOAT1 */
    case 0x22: *dxgi = DXGI_FORMAT_R32G32_FLOAT;       *size = 8;  return 1; /* FLOAT2 */
    case 0x32: *dxgi = DXGI_FORMAT_R32G32B32_FLOAT;    *size = 12; return 1; /* FLOAT3 */
    case 0x42: *dxgi = DXGI_FORMAT_R32G32B32A32_FLOAT; *size = 16; return 1; /* FLOAT4 */
    case 0x40: *dxgi = DXGI_FORMAT_B8G8R8A8_UNORM;     *size = 4;  return 1; /* D3DCOLOR */
    case 0x11: *dxgi = DXGI_FORMAT_R16_SNORM;          *size = 2;  return 1; /* NORMSHORT1 */
    case 0x21: *dxgi = DXGI_FORMAT_R16G16_SNORM;       *size = 4;  return 1; /* NORMSHORT2 */
    case 0x41: *dxgi = DXGI_FORMAT_R16G16B16A16_SNORM; *size = 8;  return 1; /* NORMSHORT4 */
    case 0x14: *dxgi = DXGI_FORMAT_R8_UNORM;           *size = 1;  return 1; /* PBYTE1 */
    case 0x24: *dxgi = DXGI_FORMAT_R8G8_UNORM;         *size = 2;  return 1; /* PBYTE2 */
    case 0x44: *dxgi = DXGI_FORMAT_R8G8B8A8_UNORM;     *size = 4;  return 1; /* PBYTE4 */
    default:   return 0;
    }
}

/* The host program's vertex layout, from the same slots: each register at its
 * declared offset in the stream 0 vertex. NORMPACKED3 (0x16, 11:11:10 signed
 * bits) has no DXGI format, so each draw copies the vertex behind its unpacked
 * normals (shadow_expand_vertices): those registers read float3s from the
 * front, and every other offset moves up by 12 bytes per packed register. A
 * declaration the host cannot take -- another stream, or a format with no
 * DXGI equivalent -- leaves has_declaration 0, and its draws are skipped and
 * counted. */
static void shadow_read_declaration(int slot, uint32_t handle)
{
    static int notes;
    struct shadow_program *p = &g_programs[slot];
    D3D8VshInput in[16];
    uint32_t object = handle & ~1u, i;
    uint32_t bad_reg = 0, bad_stream = 0, bad_format = 0;
    UINT shift;
    int n = 0, packed = 0, refused = 0;

    p->has_declaration = 0;
    p->extent = 0;
    p->packed_count = 0;
    if (!object)
        return;
    for (i = 0; i < 16u; i++)
        if (HLE_MEM32(object + 20u + i * 16u + 8u) == 0x16u)
            packed++;
    if (packed > SHADOW_MAX_PACKED)
        return;
    shift = (UINT)packed * 12u;
    packed = 0;

    for (i = 0; i < 16u; i++) {
        uint32_t attr = object + 20u + i * 16u;
        uint32_t stream = HLE_MEM32(attr), offset = HLE_MEM32(attr + 4u);
        uint32_t format = HLE_MEM32(attr + 8u);
        DXGI_FORMAT dxgi;
        UINT size;

        if (format <= 0x02u)
            continue;
        if (stream != 0u || offset > 0xFFFFu) {
            refused = 1;
        } else if (format == 0x16u) {
            p->packed_offset[packed] = offset;
            in[n].format = DXGI_FORMAT_R32G32B32_FLOAT;
            in[n].offset = 12u * (UINT)packed++;
            size = 4;
        } else if (xbox_vsdt_to_dxgi(format, &dxgi, &size)) {
            in[n].format = dxgi;
            in[n].offset = offset + shift;
        } else {
            refused = 1;
        }
        if (refused) {
            bad_reg = i;
            bad_stream = stream;
            bad_format = format;
            break;
        }
        in[n].reg = (int)i;
        if (offset + size > p->extent)
            p->extent = offset + size;
        n++;
    }

    if (!refused && n > 0 && SUCCEEDED(host_vsh_set_declaration(p->host, in, n))) {
        p->has_declaration = 1;
        p->packed_count = packed;
    } else if (refused && notes++ < 16) {
        fprintf(stderr, "[HLE-D3D8] shadow declaration 0x%08X: v%u (stream %u, format "
                "0x%02X) has no host layout; its draws are skipped\n",
                handle, bad_reg, bad_stream, bad_format);
    }
}
#endif

/* HRESULT D3DDevice_CreateVertexShader(const DWORD *pDeclaration,
 *     const DWORD *pFunction, DWORD *pHandle, DWORD Usage)
 *
 * pFunction is the XDK's compiled form: a 4-byte X_VSH_SHADER_HEADER
 * (Version, NumInst) and then NumInst instructions of 4 DWORDs of NV2A
 * microcode (Cxbx-Reloaded, XbD3D8Types.h). The host takes the microcode.
 * Only ordinary programs (Version 0x2078, 'x ') are replayed; state and
 * read/write programs set NV2A state rather than transform vertices. */
HLE_EXPORT(D3DDevice_CreateVertexShader)
{
    static int seen;
    uint32_t function = HLE_ARG(1);
#ifdef _WIN32
    uint32_t handle_va = HLE_ARG(2);
#endif

    first_call(&seen, "D3DDevice_CreateVertexShader", function);
    if (original_missing(hle_original_D3DDevice_CreateVertexShader,
                         "D3DDevice_CreateVertexShader"))
        HLE_RETURN(0x80004005u);
    HLE_CALL_ORIGINAL(D3DDevice_CreateVertexShader);
#ifdef _WIN32
    if (g_shadow && (int32_t)g_eax >= 0 && handle_va) {
        uint32_t guest = HLE_MEM32(handle_va);
        uint32_t header = function ? HLE_MEM32(function) : 0;
        int slot = shadow_program_find(guest);
        DWORD host = 0;
        HRESULT hr = E_FAIL;
        int kind;

        if (slot >= 0) {                 /* handle reused: drop the old program */
            if (g_programs[slot].kind == SHADER_HOST_PROGRAM)
                host_vsh_delete_shader(g_programs[slot].host);
        } else if (g_program_count < SHADOW_MAX_PROGRAMS) {
            slot = g_program_count++;
        }

        /* The host device's vtable has no CreateVertexShader slot; its vertex
         * program layer is called directly, with the header's count. */
        if (function && (header & 0xFFFF) == 0x2078 &&
            (header >> 16) != 0 && (header >> 16) <= 136)
            hr = host_vsh_create_shader((const DWORD *)HLE_PTR(function + 4),
                                        (int)(header >> 16), &host);
        kind = !function ? SHADER_DECLARATION
             : SUCCEEDED(hr) ? SHADER_HOST_PROGRAM : SHADER_NOT_REPLAYED;
        if (slot >= 0) {
            g_programs[slot].guest = guest;
            g_programs[slot].host = kind == SHADER_HOST_PROGRAM ? host : 0;
            g_programs[slot].kind = kind;
            g_programs[slot].has_declaration = 0;
            g_programs[slot].packed_count = 0;
        } else if (kind == SHADER_HOST_PROGRAM) {
            host_vsh_delete_shader(host);
        }
        fprintf(stderr, "[HLE-D3D8] shadow vertex shader 0x%08X: %s%s\n", guest,
                kind == SHADER_DECLARATION ? "declaration only"
                : kind == SHADER_HOST_PROGRAM ? "host program" : "program not replayed",
                slot < 0 ? " (table full, not tracked)" : "");
        note_vertex_attributes(guest);
        if (slot >= 0 && kind == SHADER_HOST_PROGRAM)
            shadow_read_declaration(slot, guest);
        /* This entry may be the selected one, recreated under the same handle. */
        if (slot >= 0 && slot == g_shadow_vs_slot)
            g_shadow_vs_kind = kind;
    }
#endif
}

/* HRESULT D3DDevice_SetVertexShader(DWORD Handle)                            */
HLE_EXPORT(D3DDevice_SetVertexShader)
{
    static int seen;
    uint32_t handle = HLE_ARG(0);

    first_call(&seen, "D3DDevice_SetVertexShader", handle);
    if (original_missing(hle_original_D3DDevice_SetVertexShader, "D3DDevice_SetVertexShader"))
        HLE_RETURN(0x80004005u);
    HLE_CALL_ORIGINAL(D3DDevice_SetVertexShader);
#ifdef _WIN32
    if (g_shadow)
        shadow_select_vertex_shader(handle);
#endif
}

/* HRESULT D3DDevice_SelectVertexShader(DWORD Handle, DWORD Address)
 * Xbox-only: select a shader, running the program previously loaded at slot
 * Address (Cxbx-Reloaded, CxbxImpl_SelectVertexShader). Handle may be 0,
 * which keeps the current declaration and is not tracked here. */
HLE_EXPORT(D3DDevice_SelectVertexShader)
{
    static int seen;
    uint32_t handle = HLE_ARG(0);

    first_call(&seen, "D3DDevice_SelectVertexShader", handle);
    if (original_missing(hle_original_D3DDevice_SelectVertexShader,
                         "D3DDevice_SelectVertexShader"))
        HLE_RETURN(0x80004005u);
    HLE_CALL_ORIGINAL(D3DDevice_SelectVertexShader);
#ifdef _WIN32
    if (g_shadow && handle)
        shadow_select_vertex_shader(handle);
#endif
}

/* void D3DDevice_SetPixelShader(DWORD Handle)
 * Handle is a shader object carrying a D3DPIXELSHADERDEF; hle_d3d8_state.c
 * reads the combiner setup out of it, because the deferred render states do
 * not follow what this call selects. Handle 0 goes back to fixed function.
 * RECOMP_HLE_D3D8_PS_PROBE=1 dumps the first few objects, which is how the
 * definition's offset was found. */
HLE_EXPORT(D3DDevice_SetPixelShader)
{
    static int seen, dumped;
#ifdef _WIN32
    uint32_t handle = HLE_ARG(0);
#endif

    first_call(&seen, "D3DDevice_SetPixelShader", HLE_ARG(0));
    if (original_missing(hle_original_D3DDevice_SetPixelShader,
                         "D3DDevice_SetPixelShader"))
        HLE_RETURN(0x80004005u);
    HLE_CALL_ORIGINAL(D3DDevice_SetPixelShader);
#ifdef _WIN32
    if (g_shadow)
        hle_d3d8_pixel_shader_selected(handle);
    if (handle && dumped < 6 && getenv("RECOMP_HLE_D3D8_PS_PROBE")) {
        int i;

        dumped++;
        fprintf(stderr, "[HLE-D3D8] SetPixelShader(0x%08X):", handle);
        for (i = 0; i < 64; i++) {
            if ((i % 8) == 0)
                fprintf(stderr, "\n  +0x%02X:", i * 4);
            fprintf(stderr, " %08X", HLE_MEM32(handle + (uint32_t)i * 4));
        }
        fprintf(stderr, "\n");
        fflush(stderr);
    }
#endif
}

/* void D3DDevice_SetVertexDataColor(INT Register, D3DCOLOR Color)
 * The current value of an input register: what a vertex program reads from a
 * register the vertex does not carry. Burnout 2 sets v3, the diffuse colour,
 * to each object's material colour before drawing it. The body writes
 * NV097_SET_VERTEX_DATA4UB, so it still runs. */
HLE_EXPORT(D3DDevice_SetVertexDataColor)
{
    static int seen;
#ifdef _WIN32
    uint32_t reg = HLE_ARG(0), color = HLE_ARG(1);
#endif

    first_call(&seen, "D3DDevice_SetVertexDataColor", HLE_ARG(0));
    if (original_missing(hle_original_D3DDevice_SetVertexDataColor,
                         "D3DDevice_SetVertexDataColor"))
        return;
    HLE_CALL_ORIGINAL(D3DDevice_SetVertexDataColor);
#ifdef _WIN32
    if (g_shadow) {
        float v[4];

        v[0] = (float)((color >> 16) & 0xFF) / 255.0f;
        v[1] = (float)((color >>  8) & 0xFF) / 255.0f;
        v[2] = (float)( color        & 0xFF) / 255.0f;
        v[3] = (float)((color >> 24) & 0xFF) / 255.0f;
        host_vsh_set_vertex_data((int)reg, v);
    }
#endif
}

/* void D3DDevice_SetVertexData2f(INT Register, float a, float b)
 * NV097_SET_VERTEX_DATA2F_M: the register becomes (a, b, 0, 1). */
HLE_EXPORT(D3DDevice_SetVertexData2f)
{
    static int seen;
#ifdef _WIN32
    uint32_t reg = HLE_ARG(0), a = HLE_ARG(1), b = HLE_ARG(2);
#endif

    first_call(&seen, "D3DDevice_SetVertexData2f", HLE_ARG(0));
    if (original_missing(hle_original_D3DDevice_SetVertexData2f,
                         "D3DDevice_SetVertexData2f"))
        return;
    HLE_CALL_ORIGINAL(D3DDevice_SetVertexData2f);
#ifdef _WIN32
    if (g_shadow) {
        float v[4] = { 0.0f, 0.0f, 0.0f, 1.0f };

        memcpy(&v[0], &a, 4);
        memcpy(&v[1], &b, 4);
        host_vsh_set_vertex_data((int)reg, v);
    }
#endif
}

/* HRESULT D3DDevice_SetTransform(D3DTRANSFORMSTATETYPE State,
 *     const D3DMATRIX *pMatrix)
 * The Xbox packs the states together -- VIEW 0, PROJECTION 1, TEXTURE0-3
 * 2-5, WORLD-WORLD3 6-9 (Cxbx-Reloaded, XbD3D8Types.h) -- where the host has
 * the PC's 2, 3, 16-19, 256-259. D3DMATRIX is 16 floats on both. */
HLE_EXPORT(D3DDevice_SetTransform)
{
    static int seen;
    uint32_t state = HLE_ARG(0);
#ifdef _WIN32
    uint32_t matrix = HLE_ARG(1);
#endif

    first_call(&seen, "D3DDevice_SetTransform", state);
    if (original_missing(hle_original_D3DDevice_SetTransform, "D3DDevice_SetTransform"))
        HLE_RETURN(0x80004005u);
    HLE_CALL_ORIGINAL(D3DDevice_SetTransform);
#ifdef _WIN32
    if (g_shadow && matrix && state < 10) {
        D3DMATRIX m;
        DWORD host_state;

        memcpy(&m, HLE_PTR(matrix), sizeof m);
        if (xbox_transform_state_to_host(state, &host_state))
            host_SetTransform(g_shadow, (D3DTRANSFORMSTATETYPE)host_state, &m);
    }
#endif
}

/* HRESULT D3DDevice_SetViewport(const D3DVIEWPORT8 *pViewport)
 * Four DWORDs and two floats on both, so it is copied whole. */
HLE_EXPORT(D3DDevice_SetViewport)
{
    static int seen;
    uint32_t viewport = HLE_ARG(0);

    first_call(&seen, "D3DDevice_SetViewport", viewport);
    if (original_missing(hle_original_D3DDevice_SetViewport, "D3DDevice_SetViewport"))
        HLE_RETURN(0x80004005u);
    HLE_CALL_ORIGINAL(D3DDevice_SetViewport);
#ifdef _WIN32
    if (g_shadow && viewport) {
        D3DVIEWPORT8 vp;

        memcpy(&vp, HLE_PTR(viewport), sizeof vp);
        /* XDK code passes Width and Height of INT_MAX to mean "the whole
         * render target" (Cxbx-Reloaded, CxbxImpl_SetViewport), and D3D11
         * would take that literally, so the viewport is kept inside the
         * current render target. */
        if (vp.X > g_target_width)
            vp.X = g_target_width;
        if (vp.Y > g_target_height)
            vp.Y = g_target_height;
        if (vp.Width > g_target_width - vp.X)
            vp.Width = g_target_width - vp.X;
        if (vp.Height > g_target_height - vp.Y)
            vp.Height = g_target_height - vp.Y;
        g_title_viewport = vp;
        g_title_viewport_set = 1;
        g_host_viewport_mode = -1;       /* the next draw picks which to use */
        shadow_viewport_constants(&vp);
    }
#endif
}

#ifdef _WIN32
/* ------------------------------------------------------------ render targets
 *
 * Titles draw into textures -- Burnout 2 renders its world several times a
 * frame into offscreen targets (and cube map faces) before it composes the
 * screen -- and without this every one of those passes, and the clear before
 * each, landed on the shadow back buffer and wiped what came before.
 *
 * An Xbox surface is a pixel container with a Parent at +20: the texture it
 * is a level of, or NULL for a surface of its own (Cxbx-Reloaded,
 * XbD3D8Types.h, X_D3DSurface). The target becomes:
 *   - the host back buffer, for the surface the XDK itself set during
 *     CreateDevice (or, if it set none, a parentless surface of the back
 *     buffer's size);
 *   - level 0 of a host render target texture, for a 2D texture's surface --
 *     the same one SetTexture binds (hle_d3d8_render_texture), so what was
 *     drawn is what the title then samples;
 *   - a scratch target of the surface's size for anything else (cube map
 *     faces, other levels, parentless surfaces of another size), so those
 *     passes at least stop drawing over the screen. Their contents are not
 *     used yet.
 * Depth: none when the title passes none. The CreateDevice depth surface with
 * the back buffer is the host device's own; any other request gets a host
 * depth surface of the target's size, since D3D11 accepts depth only at the
 * target's exact size. As on the
 * Xbox (Cxbx-Reloaded, CxbxImpl_SetRenderTarget), a new target resets the
 * viewport to all of it. */
IDirect3DTexture8 *hle_d3d8_render_texture(IDirect3DDevice8 *dev, uint32_t va);

#define SHADOW_SCRATCH 8
#define SURFACE_PARENT 20

static struct { UINT width, height; IDirect3DTexture8 *texture; } g_scratch[SHADOW_SCRATCH];
static struct { UINT width, height; IDirect3DSurface8 *surface; } g_depths[SHADOW_SCRATCH];

static void surface_measure(uint32_t va, UINT *w, UINT *h, uint32_t *fmt)
{
    uint32_t format = HLE_MEM32(va + 12), size = HLE_MEM32(va + 16);

    *fmt = (format >> 8) & 0xFF;
    if (size) {                          /* linear: width-1 and height-1 */
        *w = (size & 0xFFF) + 1;
        *h = ((size >> 12) & 0xFFF) + 1;
    } else {                             /* log2 dimensions */
        *w = 1u << ((format >> 20) & 0xF);
        *h = 1u << ((format >> 24) & 0xF);
    }
}

static IDirect3DTexture8 *scratch_target(UINT w, UINT h)
{
    int i;

    for (i = 0; i < SHADOW_SCRATCH; i++) {
        if (g_scratch[i].texture && g_scratch[i].width == w && g_scratch[i].height == h)
            return g_scratch[i].texture;
    }
    for (i = 0; i < SHADOW_SCRATCH; i++) {
        if (g_scratch[i].texture)
            continue;
        if (FAILED(host_CreateTexture(g_shadow, w, h, 1, D3DUSAGE_RENDERTARGET,
                                      D3DFMT_LIN_X8R8G8B8, D3DPOOL_DEFAULT,
                                      &g_scratch[i].texture)))
            return g_scratch[i].texture = NULL;
        g_scratch[i].width = w;
        g_scratch[i].height = h;
        return g_scratch[i].texture;
    }
    return NULL;
}

static IDirect3DSurface8 *depth_surface(UINT w, UINT h)
{
    int i;

    for (i = 0; i < SHADOW_SCRATCH; i++) {
        if (g_depths[i].surface && g_depths[i].width == w && g_depths[i].height == h)
            return g_depths[i].surface;
    }
    for (i = 0; i < SHADOW_SCRATCH; i++) {
        if (g_depths[i].surface)
            continue;
        if (FAILED(host_CreateDepthStencilSurface(g_shadow, w, h, D3DFMT_D24S8,
                                                  &g_depths[i].surface)))
            return g_depths[i].surface = NULL;
        g_depths[i].width = w;
        g_depths[i].height = h;
        return g_depths[i].surface;
    }
    return NULL;
}

static void shadow_set_render_target(uint32_t rt, uint32_t zs)
{
    static struct { uint32_t va; int kind; } seen[32];
    static int nseen;
    static uint32_t current_rt;
    IDirect3DTexture8 *texture = NULL;
    IDirect3DSurface8 *depth = NULL;
    UINT w, h, zw = 0, zh = 0;
    uint32_t fmt, zfmt = 0, parent;
    int kind, i;                         /* 0 back buffer, 1 texture, 2 scratch */

    /* A NULL target keeps the current one; only the depth surface changes. */
    if (!rt)
        rt = current_rt;
    current_rt = rt;
    g_target_sets++;
    if (!rt) {
        kind = 0;
        w = g_shadow_width;
        h = g_shadow_height;
    } else {
        surface_measure(rt, &w, &h, &fmt);
        parent = HLE_MEM32(rt + SURFACE_PARENT);
        if (parent && HLE_MEM32(parent + 4) == HLE_MEM32(rt + 4) &&
            !(HLE_MEM32(parent + 12) & 0x4)) {           /* level 0, not a cube */
            texture = hle_d3d8_render_texture(g_shadow, parent);
            kind = texture ? 1 : 2;
        } else if (g_backbuffer_va ? rt == g_backbuffer_va
                                   : (!parent && w == g_shadow_width && h == g_shadow_height)) {
            kind = 0;
        } else {
            kind = 2;
        }
        if (kind == 2) {
            texture = scratch_target(w, h);
            g_target_scratch++;
        }
        for (i = 0; i < nseen && (seen[i].va != rt || seen[i].kind != kind); i++)
            ;
        if (i == nseen && nseen < (int)(sizeof seen / sizeof seen[0])) {
            seen[nseen].va = rt;
            seen[nseen++].kind = kind;
            fprintf(stderr, "[HLE-D3D8] shadow render target 0x%08X: %ux%u format 0x%02X, "
                    "parent 0x%08X (format 0x%08X) -> %s\n", rt, w, h, fmt, parent,
                    parent ? HLE_MEM32(parent + 12) : 0,
                    kind == 0 ? "back buffer" : kind == 1 ? "render target texture"
                                                          : "scratch target");
        }
        if (kind == 2 && !texture) {
            g_target_failed++;
            kind = 0;
            w = g_shadow_width;
            h = g_shadow_height;
        }
    }

    if (zs) {
        int own;

        surface_measure(zs, &zw, &zh, &zfmt);
        g_z_scale = xbox_depth_z_scale(zfmt);
        own = g_autodepth_va ? zs == g_autodepth_va : (zw == w && zh == h);
        depth = (kind == 0 && own && g_device_depth) ? g_device_depth : depth_surface(w, h);
    } else {
        g_z_scale = 1.0f;
    }

    if (FAILED(host_SetRenderTarget(g_shadow, kind == 0 ? NULL : texture, 0, depth))) {
        /* The host keeps its old targets; go to the back buffer instead, so
         * the sizes below describe what is drawn into. */
        g_target_failed++;
        kind = 0;
        w = g_shadow_width;
        h = g_shadow_height;
        depth = zs ? g_device_depth : NULL;
        host_SetRenderTarget(g_shadow, NULL, 0, depth);
    }
    g_target_width = w;
    g_target_height = h;

    /* The Xbox resets the viewport to the whole new target. */
    g_title_viewport.X = 0;
    g_title_viewport.Y = 0;
    g_title_viewport.Width = w;
    g_title_viewport.Height = h;
    g_title_viewport.MinZ = 0.0f;
    g_title_viewport.MaxZ = 1.0f;
    g_title_viewport_set = 1;
    g_host_viewport_mode = -1;
    shadow_viewport_constants(&g_title_viewport);
}
#endif /* _WIN32 */

/* void D3DDevice_SetRenderTarget(D3DSurface *pRenderTarget,
 *     D3DSurface *pNewZStencil)                                             */
HLE_EXPORT(D3DDevice_SetRenderTarget)
{
    static int seen;
#ifdef _WIN32
    uint32_t rt = HLE_ARG(0), zs = HLE_ARG(1);
#endif

    first_call(&seen, "D3DDevice_SetRenderTarget", HLE_ARG(0));
    if (original_missing(hle_original_D3DDevice_SetRenderTarget, "D3DDevice_SetRenderTarget"))
        HLE_RETURN(0x80004005u);
    HLE_CALL_ORIGINAL(D3DDevice_SetRenderTarget);
#ifdef _WIN32
    if (g_in_create_device && rt) {
        g_backbuffer_va = rt;
        g_autodepth_va = zs;
        fprintf(stderr, "[HLE-D3D8] CreateDevice set target 0x%08X, depth 0x%08X: "
                "the frame buffer and the device's depth\n", rt, zs);
    }
    if (g_shadow)
        shadow_set_render_target(rt, zs);
#endif
}

#ifdef _WIN32
/* The vertices a program with NORMPACKED3 registers reads: each vertex copied
 * behind its unpacked normals, as shadow_read_declaration laid them out. The
 * bits are x:11, y:11, z:10, signed, divided by 1023, 1023 and 511
 * (Cxbx-Reloaded's vertex buffer conversion). Returns NULL with *failed clear
 * when the current program packs nothing, and NULL with *failed set when the
 * copy cannot be made; otherwise the copy, and *stride grows to match. */
static uint8_t *shadow_expand_vertices(const void *verts, UINT vertices, UINT *stride,
                                       int *failed)
{
    const struct shadow_program *p;
    UINT in_stride = *stride, out_stride, shift, v;
    uint8_t *out;
    int k;

    *failed = 0;
    if (!g_shadow_vs_is_program || g_shadow_vs_slot < 0)
        return NULL;
    p = &g_programs[g_shadow_vs_slot];
    if (!p->packed_count)
        return NULL;
    shift = (UINT)p->packed_count * 12u;
    out_stride = in_stride + shift;
    out = malloc((size_t)vertices * out_stride);
    if (!out) {
        *failed = 1;
        return NULL;
    }
    for (v = 0; v < vertices; v++) {
        const uint8_t *src = (const uint8_t *)verts + (size_t)v * in_stride;
        uint8_t *dst = out + (size_t)v * out_stride;

        for (k = 0; k < p->packed_count; k++) {
            uint32_t bits;
            float n[3];

            memcpy(&bits, src + p->packed_offset[k], sizeof bits);
            n[0] = (float)((int32_t)(bits << 21) >> 21) / 1023.0f;
            n[1] = (float)((int32_t)(bits << 10) >> 21) / 1023.0f;
            n[2] = (float)((int32_t)bits >> 22) / 511.0f;
            memcpy(dst + 12u * (UINT)k, n, sizeof n);
        }
        memcpy(dst + shift, src, in_stride);
    }
    *stride = out_stride;
    return out;
}

/* The host half of every non-indexed draw: the UP draw below and the vertex
 * buffer draws in hle_d3d8_vertex.c. `verts` is already a host pointer to the
 * first vertex; from_buffer only picks the counter the draw lands in. */
void hle_d3d8_shadow_draw(uint32_t xpt, uint32_t count, const void *verts,
                          uint32_t stride, int from_buffer)
{
    D3DPRIMITIVETYPE pt;
    UINT prims, host_stride = stride;
    uint8_t *loop = NULL, *expanded;
    int failed;
    HRESULT hr;

    if (!g_shadow || !verts || !stride || !count)
        return;
    if (!shadow_can_draw(xpt, stride))
        return;
    if (!xbox_primitive_to_host(xpt, count, &pt, &prims)) {
        g_draws_primitive++;
        return;
    }
    expanded = shadow_expand_vertices(verts, count, &host_stride, &failed);
    if (failed) {
        g_draws_failed++;
        return;
    }
    if (expanded)
        verts = expanded;
    if (xpt == XPT_LINELOOP) {           /* close the loop: repeat vertex 0 */
        loop = malloc((size_t)(count + 1) * host_stride);
        if (!loop) {
            free(expanded);
            g_draws_failed++;
            return;
        }
        memcpy(loop, verts, (size_t)count * host_stride);
        memcpy(loop + (size_t)count * host_stride, verts, host_stride);
        verts = loop;
    }
    hr = host_DrawPrimitiveUP(g_shadow, pt, prims, verts, host_stride);
    free(loop);
    free(expanded);
    if (FAILED(hr))
        g_draws_failed++;
    else if (from_buffer)
        g_draws_vb++;
    else
        g_draws_up++;
}

/* The host half of every indexed draw. `count` counts indices, always 16-bit
 * on the Xbox, relative to `verts`; the host wants the vertex range too, found
 * from the largest index. The host's indexed UP draw converts no primitive
 * types, so fans, polygons, quad lists and line loops are rewritten into index
 * lists here. */
void hle_d3d8_shadow_draw_indexed(uint32_t xpt, uint32_t count, const uint16_t *idx,
                                  const void *verts, uint32_t stride, int from_buffer)
{
    uint16_t *list = NULL;
    uint8_t *expanded;
    D3DPRIMITIVETYPE pt;
    UINT prims, vertices = 0, i, n = 0, host_stride = stride;
    int failed;
    HRESULT hr;

    if (!g_shadow || !idx || !verts || !stride || !count)
        return;
    if (!shadow_can_draw(xpt, stride))
        return;
    if (!xbox_primitive_to_host(xpt, count, &pt, &prims)) {
        g_draws_primitive++;
        return;
    }
    for (i = 0; i < count; i++)
        if ((UINT)idx[i] + 1 > vertices)
            vertices = (UINT)idx[i] + 1;

    switch (xpt) {
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
        list = malloc(((size_t)count + 1) * sizeof *list);
        if (list) {
            memcpy(list, idx, (size_t)count * sizeof *list);
            list[count] = idx[0];
        }
        break;
    default:
        break;
    }
    if ((xpt == XPT_TRIANGLEFAN || xpt == XPT_POLYGON || xpt == XPT_QUADLIST ||
         xpt == XPT_LINELOOP) && !list) {
        g_draws_failed++;
        return;
    }
    expanded = shadow_expand_vertices(verts, vertices, &host_stride, &failed);
    if (failed) {
        free(list);
        g_draws_failed++;
        return;
    }
    hr = host_DrawIndexedPrimitiveUP(
        g_shadow, pt, 0, vertices, prims, list ? list : idx, D3DFMT_INDEX16,
        expanded ? expanded : verts, host_stride);
    free(list);
    free(expanded);
    if (FAILED(hr))
        g_draws_failed++;
    else if (from_buffer)
        g_draws_indexed_vb++;
    else
        g_draws_indexed_up++;
}
#endif /* _WIN32 */

/* void D3DDevice_DrawVerticesUP(D3DPRIMITIVETYPE PrimitiveType,
 *     UINT VertexCount, const void *pVertexStreamZeroData,
 *     UINT VertexStreamZeroStride)                                          */
HLE_EXPORT(D3DDevice_DrawVerticesUP)
{
    static int seen;
    uint32_t xpt = HLE_ARG(0);
#ifdef _WIN32
    uint32_t count = HLE_ARG(1), data = HLE_ARG(2), stride = HLE_ARG(3);
#endif

    first_call(&seen, "D3DDevice_DrawVerticesUP", xpt);
    if (original_missing(hle_original_D3DDevice_DrawVerticesUP, "D3DDevice_DrawVerticesUP"))
        HLE_RETURN(0x80004005u);
    HLE_CALL_ORIGINAL(D3DDevice_DrawVerticesUP);
#ifdef _WIN32
    if (data)
        hle_d3d8_shadow_draw(xpt, count, HLE_PTR(data), stride, 0);
#endif
}

/* void D3DDevice_DrawIndexedVerticesUP(D3DPRIMITIVETYPE PrimitiveType,
 *     UINT VertexCount, const void *pIndexData,
 *     const void *pVertexStreamZeroData, UINT VertexStreamZeroStride)
 * VertexCount counts indices; no base vertex index applies (Cxbx-Reloaded). */
HLE_EXPORT(D3DDevice_DrawIndexedVerticesUP)
{
    static int seen;
    uint32_t xpt = HLE_ARG(0);
#ifdef _WIN32
    uint32_t count = HLE_ARG(1), index_va = HLE_ARG(2);
    uint32_t data = HLE_ARG(3), stride = HLE_ARG(4);
#endif

    first_call(&seen, "D3DDevice_DrawIndexedVerticesUP", xpt);
    if (original_missing(hle_original_D3DDevice_DrawIndexedVerticesUP,
                         "D3DDevice_DrawIndexedVerticesUP"))
        HLE_RETURN(0x80004005u);
    HLE_CALL_ORIGINAL(D3DDevice_DrawIndexedVerticesUP);
#ifdef _WIN32
    if (index_va && data)
        hle_d3d8_shadow_draw_indexed(xpt, count, (const uint16_t *)HLE_PTR(index_va),
                                     HLE_PTR(data), stride, 0);
#endif
}

#ifdef _WIN32
/* For hle_d3d8_texture.c: the shadow device (NULL when shadow mode is off)
 * and the frame count its cache ages entries by. */
IDirect3DDevice8 *hle_d3d8_shadow_device(void)
{
    return g_shadow;
}

unsigned long hle_d3d8_shadow_swaps(void)
{
    return g_shadow_swaps;
}
#endif
