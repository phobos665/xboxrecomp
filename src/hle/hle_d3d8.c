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
 * The first step toward drawing through the host renderer. CreateDevice,
 * Clear and Swap each run the title's own lifted body first (HLE_ORIGINAL),
 * so the guest device, the push buffer and every frame the executor draws
 * stay exactly as they were. Then, with the switch set, the same call is
 * repeated on a host D3D11 device in a window of its own. Registers and
 * return values are the original's, so the title sees no difference.
 *
 * It proves the title's own device parameters, clear colours and swaps reach
 * the host renderer through name-keyed replacement. It does not draw yet.
 * Without the switch these replacements only run the originals.
 *
 * Two host facts shape it:
 *   - Guest threads are real host threads, and a title may create the device
 *     on one and swap on another. DXGI's Present sends messages to the
 *     window's thread and waits for them, so a window owned by a guest thread
 *     that is itself waiting deadlocks the title. The window gets a thread of
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
    g_shadow_create_thread = GetCurrentThreadId();
    fprintf(stderr, "[HLE-D3D8] shadow device %ux%u, from the title's own "
            "CreateDevice parameters, on guest thread %lu\n",
            width, height, (unsigned long)g_shadow_create_thread);
    fflush(stderr);
}

/* Xbox D3DCLEAR_* bits: TARGET is 0xF0, one bit per channel (0x10 R, 0x20 G,
 * 0x40 B, 0x80 A); ZBUFFER is 0x01 and STENCIL 0x02. The host layer takes the
 * PC values. The first calls log the title's raw flags, so this mapping is
 * checked against what the title really passes. Two things are not honoured
 * yet: a per-channel target mask (any target bit clears all four channels, as
 * Cxbx-Reloaded also does) and clear rectangles, which the host dev_Clear
 * ignores, so a rectangle-limited clear clears the whole target. */
static DWORD xbox_clear_flags_to_host(uint32_t xbox_flags)
{
    DWORD host = 0;

    if (xbox_flags & 0xF0)
        host |= D3DCLEAR_TARGET;
    if (xbox_flags & 0x01)
        host |= D3DCLEAR_ZBUFFER;
    if (xbox_flags & 0x02)
        host |= D3DCLEAR_STENCIL;
    return host;
}

#endif /* _WIN32 */

HLE_ORIGINAL(Direct3D_CreateDevice);
HLE_ORIGINAL(D3DDevice_Clear);
HLE_ORIGINAL(D3DDevice_Swap);

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
    HLE_CALL_ORIGINAL(Direct3D_CreateDevice);
#ifdef _WIN32
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
        g_shadow->lpVtbl->Clear(g_shadow, 0, NULL, xbox_clear_flags_to_host(flags),
                                color, z, stencil);
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

        /* Which guest threads swap, against the one that created the device:
         * the case the window thread exists for. Capped, in case they alternate. */
        if (thread != g_shadow_swap_thread && g_shadow_thread_notes < 8) {
            fprintf(stderr, "[HLE-D3D8] shadow: Swap on guest thread %lu (device "
                    "created on %lu)\n", (unsigned long)thread,
                    (unsigned long)g_shadow_create_thread);
            g_shadow_thread_notes++;
        }
        g_shadow_swap_thread = thread;

        g_shadow->lpVtbl->Swap(g_shadow, 0);
        g_shadow_swaps++;
        if (!g_shadow_last_report) {
            g_shadow_last_report = now;
        } else if (now - g_shadow_last_report >= 5000) {
            fprintf(stderr, "[HLE-D3D8] shadow: %lu swaps, %lu clears, last clear "
                    "color 0x%08X\n", g_shadow_swaps, g_shadow_clears,
                    g_shadow_last_color);
            fflush(stderr);
            g_shadow_last_report = now;
        }
    }
#endif
}
