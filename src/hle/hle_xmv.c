/*
 * hle_xmv.c -- replacing the Xbox video library at its API boundary.
 *
 * A title that opens with a logo movie decodes it with XMV, which is linked
 * into the XBE and so is lifted and run like the title's own code. That
 * decoder is some of the least forgiving code in any image: hand-written MMX
 * inner loops fed by pointers that come from the graphics layer. Black faults
 * inside one of them within seconds of starting, having reached nothing else.
 *
 * Refusing to open the file does not help. Black treats a missing movie as
 * fatal -- it calls its own error callback, which asserts and halts -- and it
 * is right to, because on a real disc the file is always there.
 *
 * So the movie is replaced rather than decoded. The title asks the library to
 * create a playback, start it, and then poll it once a frame; this answers
 * those questions without decoding anything, reports the movie as finished
 * after a moment, and lets the title get on with loading. Not one instruction
 * of the title's decoder runs.
 *
 * Finding the boundary
 * --------------------
 * XbSymbolDatabase covers Direct3D, DirectSound and the application library,
 * not this one, so there were addresses and no names. scripts/section_calls.py
 * finds them a different way: each SDK library gets its own named section, so
 * the address range already says which functions belong to it, and the calls
 * that cross into that range from outside are its entry points. For Black
 * that is five functions out of a 163 KB section. The names below are the
 * XDK's own, deduced from what the callers do with each result, and they are
 * bound per title through config/extra_symbols/<title id>.json.
 *
 * The protocol, as the title uses it
 * ----------------------------------
 *   Create(flags, source, &handle)  non-zero return means failure, and the
 *                                   title then calls its error callback
 *   GetStreamInfo(handle, out)      copies the playback object's +0x40, +0x44
 *                                   and +0x48 into out +0, +4 and +0xC
 *   Start(handle)                   the title sets its own playing flag after
 *   Update(handle, surface, &status, user)
 *                                   status 0 keeps playing, 1 means finished
 *                                   and makes the title set its done flag,
 *                                   2 and 3 are paused or stopped
 *   Destroy(handle)                 the title zeroes its handle field after
 *
 * What this does not do
 * ---------------------
 * It draws nothing. The title gets a black screen for the length of the
 * movie, unless RECOMP_FMV_HOST is set, in which case the host's own decoder
 * plays the file when the title opens it and this simply keeps the title's
 * state machine happy alongside it.
 *
 * A title that reads decoded pixels back, or syncs audio to the video clock,
 * would notice. None seen so far does; if one does, the symptom will be a
 * stall rather than a fault, because every call here still answers.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "hle.h"
#include "../kernel/xbox_memory_layout.h"

/* Playback status, as the title's switch reads it. */
#define XMV_STATUS_PLAYING   0u
#define XMV_STATUS_FINISHED  1u
#define XMV_STATUS_PAUSED    2u

/* The playback object the title is handed. Only three fields are ever read by
 * it, through GetStreamInfo, and they sit where the real library puts them so
 * that a title reading them directly finds them too. The rest is ours. */
#define XMV_OBJ_SIZE     0x100u
#define XMV_OFF_WIDTH    0x40u
#define XMV_OFF_HEIGHT   0x44u
#define XMV_OFF_RATE     0x48u
/* Our own bookkeeping, past anything the library exposes. */
#define XMV_OFF_MAGIC    0xE0u
#define XMV_OFF_START_MS 0xE4u
#define XMV_OFF_FRAMES   0xE8u
#define XMV_OFF_DONE     0xECu

#define XMV_MAGIC 0x584D5648u   /* 'XMVH' */

/* How long the movie is reported to last, in milliseconds.
 *
 * Zero by default, because nothing is being decoded and the title draws
 * nothing while it waits. The wait is not free either: Black polls the
 * playback in a tight loop rather than once a frame, and a one-second movie
 * cost it 17.8 million polls of a burning core to sit through a black screen.
 *
 * A few polls still report the movie playing before it ends, so a title that
 * wants to observe playback in progress sees it. RECOMP_XMV_SECONDS gives the
 * movie a real length back, for a title that needs the time to pass.
 */
static uint32_t xmv_ms(void)
{
    static long ms = -1;
    if (ms < 0) {
        const char *env = getenv("RECOMP_XMV_SECONDS");
        double secs = env ? atof(env) : 0.0;
        if (secs < 0.0) secs = 0.0;
        ms = (long)(secs * 1000.0);
    }
    return (uint32_t)ms;
}

/* Polls that report the movie playing before it is allowed to end, so the
 * title sees a beginning even when the movie has no duration. */
#define XMV_MIN_POLLS 3u

static int xmv_enabled(void)
{
    /* On unless turned off. A title whose decoder works is better served by
     * its own, so this is the switch that gives it back. */
    return xbox_EnvSwitch("RECOMP_HLE_XMV", 1);
}

/* Milliseconds since the process started. `clock()` rather than a host timer
 * so this file stays free of platform headers -- it is the only time source
 * here and a movie's length does not need better than that. */
static uint32_t xmv_now_ms(void)
{
    return (uint32_t)((clock() * 1000ull) / (unsigned long long)CLOCKS_PER_SEC);
}

static int xmv_is_ours(uint32_t handle)
{
    return handle && HLE_MEM32(handle + XMV_OFF_MAGIC) == XMV_MAGIC;
}

/* HRESULT XMVPlaybackCreate(DWORD flags, void *source, XMVPlayback **out) */
HLE_EXPORT(XMVPlaybackCreate)
{
    uint32_t out_va = HLE_ARG(2);
    uint32_t obj;

    if (!xmv_enabled() || !out_va) {
        HLE_RETURN(0x80004005u);           /* E_FAIL: let the title decide */
        return;
    }

    obj = xbox_HeapAlloc(XMV_OBJ_SIZE, 16);
    if (!obj) {
        fprintf(stderr, "[XMV] no guest memory for a playback object\n");
        HLE_RETURN(0x8007000Eu);           /* E_OUTOFMEMORY */
        return;
    }
    memset(HLE_PTR(obj), 0, XMV_OBJ_SIZE);

    /* What the title will read back through GetStreamInfo. The dimensions are
     * the console's standard frame; the third field is a float, and a frame
     * rate is what a caller storing it next to a timer wants. */
    HLE_MEM32(obj + XMV_OFF_WIDTH)  = 640u;
    HLE_MEM32(obj + XMV_OFF_HEIGHT) = 480u;
    {
        float rate = 30.0f;
        uint32_t bits;
        memcpy(&bits, &rate, sizeof(bits));
        HLE_MEM32(obj + XMV_OFF_RATE) = bits;
    }

    HLE_MEM32(obj + XMV_OFF_MAGIC)    = XMV_MAGIC;
    HLE_MEM32(obj + XMV_OFF_START_MS) = 0;    /* not started yet */
    HLE_MEM32(obj + XMV_OFF_FRAMES)   = 0;
    HLE_MEM32(obj + XMV_OFF_DONE)     = 0;

    HLE_MEM32(out_va) = obj;

    fprintf(stderr, "[XMV] playback created at 0x%08X, reported as 640x480; "
                    "the title's decoder will not run "
                    "(RECOMP_HLE_XMV=0 gives it back)\n", obj);
    fflush(stderr);
    HLE_RETURN(0);
}

/* HRESULT XMVPlaybackStart(XMVPlayback *p) */
HLE_EXPORT(XMVPlaybackStart)
{
    uint32_t obj = HLE_ARG(0);

    if (xmv_is_ours(obj)) {
        HLE_MEM32(obj + XMV_OFF_START_MS) = xmv_now_ms();
        fprintf(stderr, "[XMV] playback started; the movie is reported as "
                        "%u ms long (RECOMP_XMV_SECONDS)\n", xmv_ms());
        fflush(stderr);
    }
    HLE_RETURN(0);
}

/* void XMVPlaybackGetStreamInfo(XMVPlayback *p, XMVSTREAMINFO *out)
 *
 * The real one copies three fields; so does this. Laid out the same way, so a
 * title that reads the object directly rather than through here agrees. */
HLE_EXPORT(XMVPlaybackGetStreamInfo)
{
    uint32_t obj = HLE_ARG(0);
    uint32_t out = HLE_ARG(1);

    if (!obj || !out)
        return;
    HLE_MEM32(out + 0x0u) = HLE_MEM32(obj + XMV_OFF_WIDTH);
    HLE_MEM32(out + 0x4u) = HLE_MEM32(obj + XMV_OFF_HEIGHT);
    HLE_MEM32(out + 0xCu) = HLE_MEM32(obj + XMV_OFF_RATE);
}

/* HRESULT XMVPlaybackUpdate(XMVPlayback *p, void *surface, DWORD *status,
 *                           void *user)
 *
 * Polled once a frame. Reports the movie playing until its time is up and
 * finished after that, which is the transition the title waits for. */
HLE_EXPORT(XMVPlaybackUpdate)
{
    uint32_t obj       = HLE_ARG(0);
    uint32_t status_va = HLE_ARG(2);
    uint32_t started, frames, elapsed;

    if (!xmv_is_ours(obj)) {
        /* Not one of ours: say nothing rather than guess, and let the title
         * take whatever path it takes for a playback it does not own. */
        HLE_RETURN(0x80004005u);
        return;
    }

    started = HLE_MEM32(obj + XMV_OFF_START_MS);
    frames  = HLE_MEM32(obj + XMV_OFF_FRAMES) + 1u;
    HLE_MEM32(obj + XMV_OFF_FRAMES) = frames;

    if (!started) {
        /* Polled before Start. Treat the first poll as the start so a title
         * that never calls Start still finishes. */
        started = xmv_now_ms();
        HLE_MEM32(obj + XMV_OFF_START_MS) = started;
    }
    elapsed = xmv_now_ms() - started;

    if (status_va) {
        uint32_t status = (frames > XMV_MIN_POLLS && elapsed >= xmv_ms())
                        ? XMV_STATUS_FINISHED : XMV_STATUS_PLAYING;
        HLE_MEM32(status_va) = status;

        if (frames <= 3u || (status == XMV_STATUS_FINISHED
                             && !HLE_MEM32(obj + XMV_OFF_DONE))) {
            fprintf(stderr, "[XMV] update #%u: status %u -> 0x%08X "
                            "(elapsed %u ms, surface 0x%08X)\n",
                    frames, status, status_va, elapsed, HLE_ARG(1));
            fflush(stderr);
        }
        if (status == XMV_STATUS_FINISHED)
            HLE_MEM32(obj + XMV_OFF_DONE) = 1u;
    }
    HLE_RETURN(0);
}

/* HRESULT XMVPlaybackDestroy(XMVPlayback *p) */
HLE_EXPORT(XMVPlaybackDestroy)
{
    uint32_t obj = HLE_ARG(0);

    if (xmv_is_ours(obj)) {
        /* The guest heap here has no free, so the object is neutered rather
         * than returned: a title that keeps a stale handle then gets the same
         * answer as one that passes a handle we never made. */
        HLE_MEM32(obj + XMV_OFF_MAGIC) = 0;
        fprintf(stderr, "[XMV] playback destroyed\n");
        fflush(stderr);
    }
    HLE_RETURN(0);
}
