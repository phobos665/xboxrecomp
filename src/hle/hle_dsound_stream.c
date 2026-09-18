/*
 * hle_dsound_stream.c -- DirectSound streams replaced by name.
 *
 * A DirectSound *buffer* holds a whole sound; a *stream* is fed packets
 * (XMEDIAPACKET) as the title decodes them from disc, and is how titles play
 * music. TimeSplitters 2 opens its music as WAV media objects
 * (XWaveFileCreateMediaObjectEx) and hands the data to a stream through
 * CDirectSoundStream_Process; its sound effects go through buffers, which
 * hle_dsound.c already sends to the host. So the effects were audible and the
 * music was not: the stream's own code drives the emulated audio chip, which
 * has no output, and the DirectSoundDoWork that would have serviced it is
 * replaced too.
 *
 * Same treatment as the buffers. The stream's format comes from the
 * DSSTREAMDESC at creation; each packet the title submits is queued and marked
 * pending; the queue plays on the host clock, decoded (PCM or Xbox ADPCM) and
 * fed to XAudio2 a little ahead of that clock; and a packet the clock has
 * passed is completed the way the console would: status XMEDIAPACKET_STATUS_
 * SUCCESS, its completed size, and then the stream's completion callback or
 * the packet's event. That completion is what makes the title submit the next
 * packet, so the stream keeps flowing.
 *
 * Streams are CDirectSoundStream objects and the title holds an
 * IDirectSoundStream interface pointer into them. The methods here take
 * `this` on the stack as their first argument (the thunks pop 4 bytes more
 * than the signature has parameters: Process is `ret 12`), and creation
 * returns the interface. Where the two differ by a constant the XDK version
 * decides, the first method call on an object binds it to the interface
 * created nearest above it.
 *
 * Not done: SYNCHPLAYBACK pauses like PAUSE; envelope and 3D settings are
 * ignored; a stream whose format the host cannot play still completes packets
 * on the clock, silently, so the title does not stall on it.
 */
#include "platform/xbox_winnt.h"
#include <stdio.h>
#include <string.h>

#include "hle.h"
#include "xbox_adpcm.h"
#include "audio_output.h"

/* kernel_bridge.c: the dispatch table, for the completion callback. */
typedef void (*recomp_func_t)(void);
recomp_func_t recomp_lookup(uint32_t xbox_va);
recomp_func_t recomp_lookup_manual(uint32_t xbox_va);
long __stdcall xbox_NtSetEvent(void *EventHandle, long *PreviousState);   /* NTSTATUS */

enum {
    MAX_STREAMS = 16,
    MAX_PACKETS = 32,
    SLOT_BASE   = 256,               /* audio_output.h: 256..271 are streams */

    TAG_PCM   = 0x0001u,
    TAG_ADPCM = 0x0069u,

    /* dsound.h */
    DSSTREAMSTATUS_READY   = 0x1u,
    DSSTREAMSTATUS_PLAYING = 0x2u,
    DSSTREAMSTATUS_PAUSED  = 0x4u,
    DSSTREAMSTATUS_STARVED = 0x8u,
    DSSTREAMPAUSE_RESUME = 0u, DSSTREAMPAUSE_PAUSE = 1u, DSSTREAMPAUSE_SYNCHPLAYBACK = 2u,

    LEAD_MS  = 400,                  /* how far ahead of the clock the host is fed */
    CHUNK_MS = 100,                  /* per submission; XAudio2 queues four */
};

/* xmo.h packet status, and HRESULTs; too wide for an enumerator. */
#define XMP_STATUS_SUCCESS 0x00000000u
#define XMP_STATUS_PENDING 0x8000000Au   /* E_PENDING */
#define XMP_STATUS_FLUSHED 0x80004004u   /* E_ABORT */
#define DS_OK              0x00000000u
#define HR_E_FAIL          0x80004005u
#define HR_E_POINTER       0x80004003u

typedef struct Packet {
    uint32_t data, size;             /* guest source bytes */
    uint32_t completed_va, status_va, context;
    uint64_t start, end;             /* decoded positions on the stream timeline */
} Packet;

typedef struct Stream {
    uint32_t iface;                  /* IDirectSoundStream the title holds */
    uint32_t object;                 /* CDirectSoundStream, bound at first use */
    uint32_t tag, channels, rate, bits, align;
    uint32_t max_packets;
    uint32_t callback, context;      /* LPFNXMEDIAOBJECTCALLBACK, its context */
    int      output;                 /* the host can play this format */
    int32_t  volume;
    int      paused;
    Packet   packets[MAX_PACKETS];
    int      head, count;
    uint64_t queued;                 /* end of the last packet queued */
    uint64_t consumed_base;          /* consumed when the clock last (re)started */
    uint64_t resumed_ms;             /* host time of that start; 0 = stopped */
    uint64_t sent;                   /* decoded bytes submitted to the host */
} Stream;

static Stream g_streams[MAX_STREAMS];
static CRITICAL_SECTION g_lock;
static INIT_ONCE g_lock_once = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK init_lock(PINIT_ONCE once, PVOID param, PVOID *ctx)
{
    (void)once; (void)param; (void)ctx;
    InitializeCriticalSection(&g_lock);
    return TRUE;
}
static void lock(void)
{
    InitOnceExecuteOnce(&g_lock_once, init_lock, NULL, NULL);
    EnterCriticalSection(&g_lock);
}
static void unlock(void) { LeaveCriticalSection(&g_lock); }
static uint64_t now_ms(void) { return GetTickCount64(); }

static int guest_readable(uint32_t va, uint32_t bytes)
{
    if (va >= 0x00010000u && (uint64_t)va + bytes <= g_xbox_total_ram)
        return 1;
    return va >= 0x80000000u && (uint64_t)va + bytes <= 0x80000000ull + 0x04000000ull;
}

static uint32_t slot_of(const Stream *s) { return SLOT_BASE + (uint32_t)(s - g_streams); }

/* Decoded bytes per second and per frame. Output is 16-bit for ADPCM. */
static uint32_t out_bits(const Stream *s) { return s->tag == TAG_ADPCM ? 16u : s->bits; }
static uint32_t frame_bytes(const Stream *s) { return s->channels * out_bits(s) / 8u; }
static uint64_t bytes_per_ms_x1000(const Stream *s)
{
    return (uint64_t)s->rate * frame_bytes(s);      /* bytes per second */
}
static uint64_t ms_to_bytes(const Stream *s, uint64_t ms)
{
    uint64_t b = ms * bytes_per_ms_x1000(s) / 1000u;
    return b - b % frame_bytes(s);
}

/* Decoded size of a packet: whole ADPCM blocks, or whole PCM frames. */
static uint64_t decoded_size(const Stream *s, uint32_t size)
{
    if (s->tag == TAG_ADPCM) {
        uint32_t block = XBOX_ADPCM_BLOCK_BYTES * s->channels;
        return (uint64_t)(size / block) * XBOX_ADPCM_BLOCK_SAMPLES * s->channels * 2u;
    }
    return size - size % frame_bytes(s);
}

static uint64_t consumed(const Stream *s, uint64_t now)
{
    uint64_t c = s->consumed_base;
    if (!s->paused && s->resumed_ms && now > s->resumed_ms)
        c += ms_to_bytes(s, now - s->resumed_ms);
    return c > s->queued ? s->queued : c;
}

/* The clock stops at the end of the data; when new data arrives after a
 * starve, or the first packet ever, it starts again from there. */
static void restart_clock(Stream *s, uint64_t now)
{
    s->consumed_base = s->queued;
    s->resumed_ms = s->paused ? 0u : now;
}

static Stream *find_by_iface(uint32_t iface)
{
    int i;
    if (!iface) return NULL;
    for (i = 0; i < MAX_STREAMS; i++)
        if (g_streams[i].iface == iface)
            return &g_streams[i];
    return NULL;
}

/* The object behind a thiscall `this`. The interface lives inside the object
 * at a fixed offset, so the first method call binds the object to the
 * interface created nearest above it. */
static Stream *find_by_object(uint32_t object)
{
    static int said;
    Stream *best = NULL;
    uint32_t best_d = 0x100u;
    int i;

    if (!object) return NULL;
    for (i = 0; i < MAX_STREAMS; i++)
        if (g_streams[i].object == object)
            return &g_streams[i];
    for (i = 0; i < MAX_STREAMS; i++) {
        Stream *s = &g_streams[i];
        if (s->iface && !s->object && s->iface >= object && s->iface - object < best_d) {
            best = s;
            best_d = s->iface - object;
        }
    }
    if (best) {
        best->object = object;
        if (said++ < 4)
            fprintf(stderr, "[DSOUND] stream %08X: object %08X (interface +0x%X)\n",
                    best->iface, object, best_d);
    }
    return best;
}

/* Complete one packet: status, size, then the callback or the event. The
 * callback is LPFNXMEDIAOBJECTCALLBACK, __stdcall (pStreamContext,
 * pPacketContext, dwStatus); its `ret 12` pops the arguments and the dummy
 * return address, as kernel_run_dpc relies on for DPCs. */
static void complete(Stream *s, const Packet *p, uint32_t status, uint32_t size)
{
    if (p->status_va && guest_readable(p->status_va, 4u))
        HLE_MEM32(p->status_va) = status;
    if (p->completed_va && guest_readable(p->completed_va, 4u))
        HLE_MEM32(p->completed_va) = size;
    if (s->callback) {
        recomp_func_t fn = recomp_lookup(s->callback);
        if (!fn) fn = recomp_lookup_manual(s->callback);
        if (fn) {
            g_esp -= 4; HLE_MEM32(g_esp) = status;
            g_esp -= 4; HLE_MEM32(g_esp) = p->context;
            g_esp -= 4; HLE_MEM32(g_esp) = s->context;
            g_esp -= 4; HLE_MEM32(g_esp) = 0;
            fn();
        } else {
            static int said;
            if (!said++)
                fprintf(stderr, "[DSOUND] stream callback 0x%08X not in dispatch; "
                        "packets complete without it\n", s->callback);
        }
    } else if (p->context) {
        xbox_NtSetEvent((void *)(uintptr_t)p->context, NULL);
    }
}

static void complete_passed(Stream *s, uint64_t now)
{
    uint64_t c = consumed(s, now);
    while (s->count > 0) {
        Packet *p = &s->packets[s->head];
        if (p->end > c)
            break;
        {
            Packet done = *p;
            s->head = (s->head + 1) % MAX_PACKETS;
            s->count--;
            complete(s, &done, XMP_STATUS_SUCCESS, done.size);
        }
    }
}

/* Feed the host from `sent` up to the clock plus the lead, in chunks. */
static void pump(Stream *s, uint64_t now)
{
    static uint8_t pcm[160000];
    uint64_t target;
    int guard = 0;

    if (!s->output || s->paused || !s->resumed_ms)
        return;
    target = consumed(s, now) + ms_to_bytes(s, LEAD_MS);
    if (target > s->queued)
        target = s->queued;
    while (s->sent < target && guard++ < 64) {
        Packet *p = NULL;
        int i;
        uint64_t within, want;
        uint32_t bytes;

        for (i = 0; i < s->count; i++) {
            Packet *q = &s->packets[(s->head + i) % MAX_PACKETS];
            if (s->sent >= q->start && s->sent < q->end) { p = q; break; }
        }
        if (!p) {                    /* a gap: packets already completed */
            s->sent = target;
            break;
        }
        within = s->sent - p->start;
        want = ms_to_bytes(s, CHUNK_MS);
        if (want > p->end - s->sent) want = p->end - s->sent;
        if (want > target - s->sent) want = target - s->sent;
        if (want > sizeof pcm) want = sizeof pcm;
        if (s->tag == TAG_ADPCM) {
            uint32_t block = XBOX_ADPCM_BLOCK_BYTES * s->channels;
            uint32_t dblock = XBOX_ADPCM_BLOCK_SAMPLES * s->channels * 2u;
            uint32_t n = (uint32_t)(want / dblock), k;
            if (n == 0u) n = 1u;
            bytes = 0u;
            for (k = 0; k < n; k++) {
                uint32_t src = p->data + (uint32_t)((within / dblock) + k) * block;
                int16_t decoded[XBOX_ADPCM_BLOCK_SAMPLES * 2];
                if (!guest_readable(src, block) ||
                    !xbox_adpcm_decode_block((const uint8_t *)HLE_PTR(src), block,
                                             s->channels, decoded,
                                             XBOX_ADPCM_BLOCK_SAMPLES * 2u)) {
                    s->output = 0;   /* bad data: keep the clock, drop the sound */
                    recomp_audio_output_reset_voice(slot_of(s));
                    return;
                }
                memcpy(pcm + bytes, decoded, dblock);
                bytes += dblock;
            }
        } else {
            bytes = (uint32_t)want - (uint32_t)(want % frame_bytes(s));
            if (!bytes || !guest_readable(p->data + (uint32_t)within, bytes)) {
                s->output = 0;
                recomp_audio_output_reset_voice(slot_of(s));
                return;
            }
            memcpy(pcm, HLE_PTR(p->data + (uint32_t)within), bytes);
        }
        recomp_audio_output_submit(slot_of(s), pcm, bytes, s->rate, s->channels,
                                   out_bits(s), s->volume);
        s->sent += bytes;
    }
}

/* Drop what the host has queued and start feeding again from the clock. */
static void refeed(Stream *s, uint64_t now)
{
    recomp_audio_output_reset_voice(slot_of(s));
    s->sent = consumed(s, now);
    pump(s, now);
}

static void tick(Stream *s, uint64_t now)
{
    complete_passed(s, now);
    pump(s, now);
}

void hle_dsound_stream_tick(uint64_t now)
{
    int i;
    lock();
    for (i = 0; i < MAX_STREAMS; i++)
        if (g_streams[i].iface)
            tick(&g_streams[i], now);
    unlock();
}

/* Record a stream the title just created, from its DSSTREAMDESC:
 *   +0 dwFlags  +4 dwMaxAttachedPackets  +8 lpwfxFormat  +C lpMixBins
 *   +10 lpfnCallback  +14 lpvContext
 * and the WAVEFORMATEX it names. Idempotent: DirectSoundCreateStream calls
 * CDirectSound_CreateSoundStream, and both are replaced. */
static void record_stream(uint32_t iface, uint32_t desc)
{
    static int said;
    Stream *s;
    uint32_t wfx, max_packets, callback, context;
    int i;

    if (!iface || find_by_iface(iface))
        return;
    if (!desc || !guest_readable(desc, 0x18u))
        return;
    max_packets = HLE_MEM32(desc + 4u);
    wfx         = HLE_MEM32(desc + 8u);
    callback    = HLE_MEM32(desc + 0x10u);
    context     = HLE_MEM32(desc + 0x14u);
    if (!wfx || !guest_readable(wfx, 16u))
        return;

    s = NULL;
    for (i = 0; i < MAX_STREAMS; i++)
        if (!g_streams[i].iface) { s = &g_streams[i]; break; }
    if (!s) {
        if (!said++)
            fprintf(stderr, "[DSOUND] more than %d streams; the rest are not modelled\n",
                    MAX_STREAMS);
        return;
    }
    memset(s, 0, sizeof *s);
    s->iface       = iface;
    s->tag         = HLE_MEM32(wfx) & 0xFFFFu;
    s->channels    = HLE_MEM32(wfx) >> 16;
    s->rate        = HLE_MEM32(wfx + 4u);
    s->align       = HLE_MEM32(wfx + 12u) & 0xFFFFu;
    s->bits        = HLE_MEM32(wfx + 12u) >> 16;
    s->max_packets = max_packets && max_packets < MAX_PACKETS ? max_packets : MAX_PACKETS;
    s->callback    = callback;
    s->context     = context;
    s->output = s->channels >= 1u && s->channels <= 2u &&
                s->rate >= 1000u && s->rate <= 200000u &&
                ((s->tag == TAG_PCM && (s->bits == 8u || s->bits == 16u)) ||
                 (s->tag == TAG_ADPCM && s->bits == 4u));
    if (said < 8) {
        said++;
        fprintf(stderr, "[DSOUND] stream %08X: tag %04X %u ch %u Hz %u bit align %u, "
                "%u packets, callback %08X %s\n", iface, s->tag, s->channels,
                s->rate, s->bits, s->align, s->max_packets, callback,
                s->output ? "output" : "clock only");
        fflush(stderr);
    }
}

HLE_ORIGINAL(DirectSoundCreateStream);
HLE_ORIGINAL(CDirectSound_CreateSoundStream);
HLE_ORIGINAL(CDirectSoundStream_SetVolume);

/* HRESULT DirectSoundCreateStream(LPCDSSTREAMDESC pdssd,
 *     LPDIRECTSOUNDSTREAM *ppStream) */
HLE_EXPORT(DirectSoundCreateStream)
{
    uint32_t desc = HLE_ARG(0), out = HLE_ARG(1);

    if (!hle_original_DirectSoundCreateStream)
        HLE_RETURN(HR_E_FAIL);
    HLE_CALL_ORIGINAL(DirectSoundCreateStream);
    if ((int32_t)g_eax >= 0 && out && guest_readable(out, 4u)) {
        lock();
        record_stream(HLE_MEM32(out), desc);
        unlock();
    }
}

/* HRESULT CDirectSound::CreateSoundStream(this, LPCDSSTREAMDESC pdssd,
 *     LPDIRECTSOUNDSTREAM *ppStream, LPUNKNOWN pUnkOuter) */
HLE_EXPORT(CDirectSound_CreateSoundStream)
{
    uint32_t desc = HLE_ARG(1), out = HLE_ARG(2);

    if (!hle_original_CDirectSound_CreateSoundStream)
        HLE_RETURN(HR_E_FAIL);
    HLE_CALL_ORIGINAL(CDirectSound_CreateSoundStream);
    if ((int32_t)g_eax >= 0 && out && guest_readable(out, 4u)) {
        lock();
        record_stream(HLE_MEM32(out), desc);
        unlock();
    }
}

/* HRESULT CDirectSoundStream::Process(this, LPCXMEDIAPACKET pInputBuffer,
 *     LPCXMEDIAPACKET pOutputBuffer)
 * XMEDIAPACKET: +0 pvBuffer +4 dwMaxSize +8 pdwCompletedSize +C pdwStatus
 *               +10 hCompletionEvent / pContext  +14 prtTimestamp */
HLE_EXPORT(CDirectSoundStream_Process)
{
    uint32_t object = HLE_ARG(0), packet = HLE_ARG(1);
    uint64_t now = now_ms();
    Stream *s;
    Packet *p;
    static int said;

    if (!packet || !guest_readable(packet, 0x18u))
        HLE_RETURN(HR_E_POINTER);
    lock();
    s = find_by_object(object);
    if (!s) {
        unlock();
        if (said++ < 4)
            fprintf(stderr, "[DSOUND] stream Process on unknown object %08X\n", object);
        HLE_RETURN(DS_OK);
    }
    complete_passed(s, now);
    if (s->count >= (int)s->max_packets) {
        unlock();
        HLE_RETURN(HR_E_FAIL);          /* the title asks GetStatus for READY first */
    }
    p = &s->packets[(s->head + s->count) % MAX_PACKETS];
    p->data         = HLE_MEM32(packet);
    p->size         = HLE_MEM32(packet + 4u);
    p->completed_va = HLE_MEM32(packet + 8u);
    p->status_va    = HLE_MEM32(packet + 0xCu);
    p->context      = HLE_MEM32(packet + 0x10u);
    if (consumed(s, now) >= s->queued)
        restart_clock(s, now);       /* first packet, or playing again after a starve */
    p->start = s->queued;
    p->end   = s->queued + decoded_size(s, p->size);
    s->queued = p->end;
    s->count++;
    if (p->status_va && guest_readable(p->status_va, 4u))
        HLE_MEM32(p->status_va) = XMP_STATUS_PENDING;
    if (said < 4) {
        said++;
        fprintf(stderr, "[DSOUND] stream %08X: packet %u bytes at %08X (%d queued)\n",
                s->iface, p->size, p->data, s->count);
    }
    pump(s, now);
    unlock();
    HLE_RETURN(DS_OK);
}

/* HRESULT CDirectSoundStream::Flush(this). Every pending packet comes back
 * flushed, and the clock moves to the end of the data. */
HLE_EXPORT(CDirectSoundStream_Flush)
{
    uint64_t now = now_ms();
    Stream *s;

    lock();
    s = find_by_object(HLE_ARG(0));
    if (s) {
        Packet flushed[MAX_PACKETS];
        int n = 0, i;
        while (s->count > 0) {
            flushed[n++] = s->packets[s->head];
            s->head = (s->head + 1) % MAX_PACKETS;
            s->count--;
        }
        s->consumed_base = s->queued;
        s->resumed_ms = 0u;
        s->sent = s->queued;
        recomp_audio_output_reset_voice(slot_of(s));
        for (i = 0; i < n; i++)
            complete(s, &flushed[i], XMP_STATUS_FLUSHED, 0u);
        (void)now;
    }
    unlock();
    HLE_RETURN(DS_OK);
}

/* HRESULT CDirectSoundStream::Discontinuity(this). Marks the end of the
 * data; the packets already queued play out as they are. */
HLE_EXPORT(CDirectSoundStream_Discontinuity)
{
    HLE_RETURN(DS_OK);
}

/* HRESULT CDirectSoundStream::Pause(this, DWORD dwPause) */
HLE_EXPORT(CDirectSoundStream_Pause)
{
    uint32_t mode = HLE_ARG(1);
    uint64_t now = now_ms();
    Stream *s;

    lock();
    s = find_by_object(HLE_ARG(0));
    if (s) {
        if (mode == DSSTREAMPAUSE_RESUME) {
            if (s->paused) {
                s->paused = 0;
                s->resumed_ms = s->consumed_base < s->queued ? now : 0u;
                if (!s->resumed_ms)
                    s->consumed_base = s->queued;
                refeed(s, now);
            }
        } else {                      /* PAUSE, and SYNCHPLAYBACK treated the same */
            if (!s->paused) {
                s->consumed_base = consumed(s, now);
                s->paused = 1;
                s->resumed_ms = 0u;
                recomp_audio_output_reset_voice(slot_of(s));
                s->sent = s->consumed_base;
            }
        }
    }
    unlock();
    HLE_RETURN(DS_OK);
}

/* HRESULT CDirectSoundStream::GetStatus(this, DWORD *pdwStatus) */
HLE_EXPORT(CDirectSoundStream_GetStatus__r2)
{
    uint32_t out = HLE_ARG(1), status = 0u;
    uint64_t now = now_ms();
    Stream *s;

    if (!out || !guest_readable(out, 4u))
        HLE_RETURN(HR_E_POINTER);
    lock();
    s = find_by_object(HLE_ARG(0));
    if (s) {
        tick(s, now);
        if (s->count < (int)s->max_packets)
            status |= DSSTREAMSTATUS_READY;
        if (s->paused)
            status |= DSSTREAMSTATUS_PAUSED;
        else if (s->count > 0)
            status |= DSSTREAMSTATUS_PLAYING;
        else if (s->queued)
            status |= DSSTREAMSTATUS_STARVED;
    } else {
        status = DSSTREAMSTATUS_READY;
    }
    unlock();
    HLE_MEM32(out) = status;
    HLE_RETURN(DS_OK);
}

/* HRESULT CDirectSoundStream::SetVolume(this, LONG lVolume), hundredths of a
 * decibel. The title's own code keeps its voice settings; the value is kept
 * here for the host mix. */
HLE_EXPORT(CDirectSoundStream_SetVolume)
{
    uint32_t object = HLE_ARG(0), volume = HLE_ARG(1);
    Stream *s;

    lock();
    s = find_by_object(object);
    if (s)
        s->volume = (int32_t)volume;
    unlock();
    if (hle_original_CDirectSoundStream_SetVolume) {
        HLE_CALL_ORIGINAL(CDirectSoundStream_SetVolume);
        return;
    }
    HLE_RETURN(DS_OK);
}
