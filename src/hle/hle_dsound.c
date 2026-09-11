/*
 * hle_dsound.c -- DirectSound buffers replaced by name.
 *
 * Adapted from doaxbv-re (https://github.com/NoRain211/doaxbv-re,
 * recomp-runtime/dsound_service_adapter.c), GPL-3.0.
 *
 * The game's own DirectSound drives an emulated MCPX audio chip, and every
 * piece of chip behaviour it relies on has to be found before it works: the
 * codec-ready bit, the DSP doorbells, starting the chip, and then its
 * interrupt, whose handler still declined it. Burnout 2 stops a sound before
 * its attract movie and waits for IDirectSoundBuffer_GetStatus to stop saying
 * PLAYING -- 4.5 million polls, forever.
 *
 * doaxbv-re's answer, reused here: replace the buffer functions and keep each
 * buffer's play state (playing, cursor, loop, frequency) in a model on the
 * host clock (dsound_buffer_model.c), sending the samples to XAudio2
 * (audio_output_xaudio2.cpp). GetStatus then never waits on hardware.
 *
 * Differences from doaxbv-re:
 *  - keyed by XDK function name through tools.xdk_symbols, not by one title's
 *    addresses, so any title on a covered XDK gets it;
 *  - the public IDirectSoundBuffer_* entry points are replaced; creation,
 *    SetBufferData, SetVolume and the rest still run the game's own code,
 *    which keeps the settings object this reads current;
 *  - a buffer whose format cannot be played still gets a clock, so status and
 *    position always advance: doaxbv-re fell back to the original function,
 *    which here is the path that hangs;
 *  - a lock, since nothing guarantees one guest thread.
 *
 * Where the fields live (XDK 5344, Burnout 2). The interface pointer the game
 * holds is the buffer object + 0x1C, and the object keeps two settings
 * pointers: +0x1C for the buffer (data, size, play and loop regions -- read
 * by CDirectSoundBufferSettings_SetBufferData, SetLoopRegion and the
 * play-region helper) and +0x10 for the voice (format, rate, alignment,
 * volume -- written by the format packer and CDirectSoundVoice_SetVolume).
 * doaxbv-re reads all of it through the first, which is only right if both
 * point at one object; each field is read here from the one the game's own
 * code writes it to. The offsets inside match doaxbv-re's.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>

#include "hle.h"
#include "dsound_buffer_model.h"
#include "xbox_adpcm.h"
#include "audio_output.h"

enum {
    SET_FORMAT     = 0x0Cu,   /* tag | channels << 16 | bits << 24 */
    SET_RATE       = 0x10u,
    SET_ALIGN      = 0x14u,
    SET_VOLUME     = 0x1Cu,   /* hundredths of a dB, headroom applied */
    SET_DATA       = 0xB8u,
    SET_SIZE       = 0xBCu,
    SET_PLAY_START = 0xC0u,
    SET_PLAY_LEN   = 0xC4u,
    SET_LOOP_START = 0xC8u,
    SET_LOOP_LEN   = 0xCCu,

    TAG_PCM   = 0x0001u,
    TAG_ADPCM = 0x0069u,
    SLOTS     = 256u,
};

typedef struct Buffer {
    uint32_t iface;              /* the IDirectSoundBuffer the game holds */
    uint32_t data, size, format;
    int      output;             /* samples we can send to the host */
    RecompDsoundBufferModel model;        /* what the game is told */
    RecompDsoundBufferModel output_model; /* what has been sent */
} Buffer;

static Buffer g_buffers[SLOTS];
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

/* Buffer settings: [iface] = [object + 0x1C]. Voice settings: [object + 0x10],
 * i.e. [iface - 0x0C]. */
static int voice_field(uint32_t offset)
{
    return offset == SET_FORMAT || offset == SET_RATE ||
           offset == SET_ALIGN || offset == SET_VOLUME;
}

static uint32_t setting(uint32_t iface, uint32_t offset)
{
    uint32_t object = voice_field(offset) ? HLE_MEM32(iface - 0x0Cu)
                                          : HLE_MEM32(iface);
    return HLE_MEM32(object + offset);
}

static uint32_t slot_of(const Buffer *b) { return (uint32_t)(b - g_buffers); }

static void retire(Buffer *b)
{
    recomp_audio_output_reset_voice(slot_of(b));
    memset(b, 0, sizeof *b);
}

/* Send the samples the output clock has consumed since the last pump. */
static void pump(Buffer *b, uint64_t now)
{
    RecompDsoundBufferModel *out = &b->output_model;
    uint32_t channels, bits, offset = 0u, bytes;
    int adpcm;
    static uint8_t pcm[80000];

    if (!b->output || !out->playing || now <= out->last_ms ||
        now - out->last_ms < 10u)
        return;
    if (b->data != setting(b->iface, SET_DATA) ||
        b->size != setting(b->iface, SET_SIZE) ||
        b->format != setting(b->iface, SET_FORMAT)) {
        retire(b);
        return;
    }
    channels = (b->format >> 16) & 0xFFu;
    bits = b->format >> 24;
    adpcm = (b->format & 0xFFFFu) == TAG_ADPCM;
    bytes = recomp_dsound_buffer_consume(out, now, &offset);
    if (bytes == 0u || b->data == 0u || bytes > sizeof pcm)
        return;

    if (adpcm) {
        uint32_t block_bytes = XBOX_ADPCM_BLOCK_BYTES * channels;
        uint32_t decoded_bytes = XBOX_ADPCM_BLOCK_SAMPLES * channels * 2u;
        uint32_t written;
        for (written = 0u; written < bytes;) {
            int16_t decoded[XBOX_ADPCM_BLOCK_SAMPLES * 2];
            uint32_t within = offset % decoded_bytes;
            uint32_t count = decoded_bytes - within;
            if (count > bytes - written) count = bytes - written;
            if (!xbox_adpcm_decode_block(
                    (const uint8_t *)HLE_PTR(b->data + offset / decoded_bytes * block_bytes),
                    block_bytes, channels, decoded, XBOX_ADPCM_BLOCK_SAMPLES * 2u)) {
                b->output = 0;          /* bad data: keep the clock, stop the sound */
                recomp_audio_output_reset_voice(slot_of(b));
                return;
            }
            memcpy(pcm + written, (uint8_t *)decoded + within, count);
            written += count;
            offset += count;
            if (offset == out->size_bytes) offset = out->loop_start_bytes;
        }
        bits = 16u;
    } else {
        uint32_t written;
        for (written = 0u; written < bytes;) {
            uint32_t count = out->size_bytes - offset;
            if (count > bytes - written) count = bytes - written;
            memcpy(pcm + written, HLE_PTR(b->data + offset), count);
            written += count;
            offset += count;
            if (offset == out->size_bytes) offset = out->loop_start_bytes;
        }
    }
    recomp_audio_output_submit(slot_of(b), pcm, bytes, out->sample_rate,
                               channels, bits,
                               (int32_t)setting(b->iface, SET_VOLUME));
}

static Buffer *find(uint32_t iface)
{
    uint32_t i;
    if (iface == 0u) return NULL;
    for (i = 0; i < SLOTS; i++)
        if (g_buffers[i].iface == iface)
            return &g_buffers[i];
    return NULL;
}

/* The buffer's model, created or refreshed from the settings object. NULL when
 * nothing about it can be clocked (no rate or alignment worth trusting). */
static Buffer *model_for(uint32_t iface, uint64_t now)
{
    Buffer *b = find(iface);
    uint32_t size, data, format, tag, channels, bits, align, rate, loop_start;
    uint32_t decoded_size, decoded_loop, block_align;
    int output;
    uint32_t i;

    if (iface == 0u || HLE_MEM32(iface) == 0u || HLE_MEM32(iface - 0x0Cu) == 0u)
        return NULL;
    size       = setting(iface, SET_SIZE);
    data       = setting(iface, SET_DATA);
    format     = setting(iface, SET_FORMAT);
    align      = setting(iface, SET_ALIGN);
    rate       = setting(iface, SET_RATE);
    loop_start = setting(iface, SET_LOOP_START);
    tag        = format & 0xFFFFu;
    channels   = (format >> 16) & 0xFFu;
    bits       = format >> 24;

    if (b && (b->data != data || b->size != size || b->format != format)) {
        retire(b);
        b = NULL;
    }
    if (align == 0u || rate < 1000u || rate > 200000u)
        return NULL;
    /* Clock whole blocks only. Burnout 2's attract-movie stream is a stereo
     * ADPCM ring of 7,876,608 bytes -- not a multiple of its 72-byte block --
     * and requiring an exact multiple left the one buffer the game waits on
     * with no model and no sound. The tail is less than one block. b->size
     * below keeps the raw size, so a real change is still noticed. */
    {
        uint32_t raw_size = size;
        size -= size % align;
        if (size == 0u)
            return NULL;
        if (b) {
            b->size = raw_size;
        }
        if (loop_start >= size)
            loop_start = 0u;
        (void)raw_size;
    }
    if (loop_start >= size || loop_start % align != 0u)
        loop_start = 0u;

    output = channels >= 1u && channels <= 2u &&
        ((tag == TAG_PCM && (bits == 8u || bits == 16u) &&
          align == channels * bits / 8u) ||
         (tag == TAG_ADPCM && bits == 4u &&
          align == XBOX_ADPCM_BLOCK_BYTES * channels));

    /* ADPCM is clocked in decoded PCM bytes, as the output consumes it. */
    if (tag == TAG_ADPCM && output) {
        uint64_t d = (uint64_t)(size / align) * XBOX_ADPCM_BLOCK_SAMPLES * channels * 2u;
        if (d > UINT32_MAX) return NULL;
        decoded_size = (uint32_t)d;
        decoded_loop = loop_start / align * XBOX_ADPCM_BLOCK_SAMPLES * channels * 2u;
        block_align = channels * 2u;
    } else {
        decoded_size = size;
        decoded_loop = loop_start;
        block_align = align;
    }

    if (b) {
        if (b->model.loop_start_bytes != decoded_loop) {
            pump(b, now);
            recomp_dsound_buffer_cursor(&b->model, now);
            recomp_audio_output_reset_voice(slot_of(b));
            b->model.loop_start_bytes = decoded_loop;
            b->output_model = b->model;
        }
        return b;
    }
    for (i = 0; i < SLOTS; i++) {
        if (g_buffers[i].iface == 0u) {
            b = &g_buffers[i];
            if (recomp_dsound_buffer_configure(&b->model, decoded_size, rate,
                                               block_align, now) != 0u)
                return NULL;
            b->iface = iface;
            b->data = data;
            b->size = size;
            b->format = format;
            b->output = output;
            b->model.loop_start_bytes = decoded_loop;
            b->output_model = b->model;
            return b;
        }
    }
    {
        static int said;
        if (!said++) {
            fprintf(stderr, "[DSOUND] more than %u buffers in play; "
                            "the rest are not modelled\n", SLOTS);
            fflush(stderr);
        }
    }
    return NULL;
}

static uint64_t now_ms(void) { return GetTickCount64(); }

/* After a control change, restart the output from the model unless this is a
 * Play that simply continues a sound already playing. */
static void resync_output(Buffer *b, int continuing)
{
    if (!continuing)
        recomp_audio_output_reset_voice(slot_of(b));
    if (continuing)
        b->output_model.play_flags = b->model.play_flags;
    else
        b->output_model = b->model;
}

/* HRESULT IDirectSoundBuffer_Play(this, reserved1, reserved2, flags) */
HLE_EXPORT(IDirectSoundBuffer_Play)
{
    uint32_t iface = HLE_ARG(0), flags = HLE_ARG(3), result;
    uint64_t now = now_ms();
    static unsigned said;
    Buffer *b;
    int continuing;

    lock();
    b = model_for(iface, now);
    if (said < 16) {
        said++;
        fprintf(stderr, "[DSOUND] Play buffer=%08X format=%08X bytes=%u rate=%u "
                        "flags=%u %s\n", iface,
                iface && HLE_MEM32(iface) ? setting(iface, SET_FORMAT) : 0u,
                iface && HLE_MEM32(iface) ? setting(iface, SET_SIZE) : 0u,
                iface && HLE_MEM32(iface) ? setting(iface, SET_RATE) : 0u, flags,
                !b ? "not modelled" : b->output ? "output" : "clock only");
        fflush(stderr);
    }
    if (!b) {
        unlock();
        HLE_RETURN(iface ? RECOMP_DSOUND_OK : RECOMP_DSOUND_POINTER_ERROR);
    }
    pump(b, now);
    recomp_dsound_buffer_cursor(&b->model, now);
    continuing = b->model.playing && b->output_model.playing &&
                 !(flags & RECOMP_DSOUND_PLAY_FROMSTART);
    result = recomp_dsound_buffer_play(&b->model, flags, now);
    if (result == RECOMP_DSOUND_OK)
        resync_output(b, continuing);
    unlock();
    HLE_RETURN(result);
}

static void stop(uint32_t iface)
{
    uint64_t now = now_ms();
    Buffer *b;

    lock();
    b = find(iface);
    if (b) {
        pump(b, now);
        recomp_dsound_buffer_stop(&b->model, now);
        resync_output(b, 0);
    }
    unlock();
}

/* HRESULT IDirectSoundBuffer_Stop(this) */
HLE_EXPORT(IDirectSoundBuffer_Stop)
{
    stop(HLE_ARG(0));
    HLE_RETURN(HLE_ARG(0) ? RECOMP_DSOUND_OK : RECOMP_DSOUND_POINTER_ERROR);
}

/* HRESULT IDirectSoundBuffer_StopEx(this, REFERENCE_TIME rtTimeStamp, flags).
 * The time stamp and envelope flags ask for a scheduled or released stop; an
 * immediate one is what the game can observe, and all it waits for. */
HLE_EXPORT(IDirectSoundBuffer_StopEx)
{
    stop(HLE_ARG(0));
    HLE_RETURN(HLE_ARG(0) ? RECOMP_DSOUND_OK : RECOMP_DSOUND_POINTER_ERROR);
}

/* HRESULT IDirectSoundBuffer_GetStatus(this, DWORD *status) */
HLE_EXPORT(IDirectSoundBuffer_GetStatus)
{
    uint32_t iface = HLE_ARG(0), out = HLE_ARG(1), status = 0u;
    uint64_t now = now_ms();
    Buffer *b;

    if (out == 0u)
        HLE_RETURN(RECOMP_DSOUND_POINTER_ERROR);
    lock();
    b = find(iface);
    if (b) {
        pump(b, now);
        recomp_dsound_buffer_cursor(&b->model, now);
        if (b->model.playing)
            status = 1u | ((b->model.play_flags & RECOMP_DSOUND_PLAY_LOOPING) ? 4u : 0u);
    }
    unlock();
    HLE_MEM32(out) = status;          /* never played here: not playing */
    HLE_RETURN(RECOMP_DSOUND_OK);
}

/* HRESULT IDirectSoundBuffer_GetCurrentPosition(this, DWORD *play, DWORD *write) */
HLE_EXPORT(IDirectSoundBuffer_GetCurrentPosition)
{
    uint32_t iface = HLE_ARG(0), cursor = 0u, i;
    uint64_t now = now_ms();
    Buffer *b;

    lock();
    b = model_for(iface, now);
    if (b) {
        pump(b, now);
        cursor = recomp_dsound_buffer_cursor(&b->model, now);
        /* ADPCM is clocked in decoded bytes; the game asks in source bytes. */
        if ((b->format & 0xFFFFu) == TAG_ADPCM && b->output)
            cursor = cursor / (XBOX_ADPCM_BLOCK_SAMPLES * b->model.block_align) *
                     (XBOX_ADPCM_BLOCK_BYTES * b->model.block_align / 2u);
    }
    unlock();
    for (i = 1u; i <= 2u; i++) {
        uint32_t out = HLE_ARG(i);
        if (out != 0u)
            HLE_MEM32(out) = cursor;
    }
    HLE_RETURN(RECOMP_DSOUND_OK);
}

/* HRESULT IDirectSoundBuffer_SetCurrentPosition(this, DWORD position) */
HLE_EXPORT(IDirectSoundBuffer_SetCurrentPosition)
{
    uint32_t iface = HLE_ARG(0), position = HLE_ARG(1), result = RECOMP_DSOUND_OK;
    uint64_t now = now_ms();
    Buffer *b;

    lock();
    b = model_for(iface, now);
    if (b) {
        pump(b, now);
        if ((b->format & 0xFFFFu) == TAG_ADPCM && b->output) {
            uint32_t block = XBOX_ADPCM_BLOCK_BYTES * b->model.block_align / 2u;
            if (position >= b->size || position % block != 0u)
                result = RECOMP_DSOUND_INVALID_PARAM;
            else
                position = position / block * XBOX_ADPCM_BLOCK_SAMPLES *
                           b->model.block_align;
        }
        if (result == RECOMP_DSOUND_OK)
            result = recomp_dsound_buffer_set_position(&b->model, position, now);
        if (result == RECOMP_DSOUND_OK)
            resync_output(b, 0);
    }
    unlock();
    HLE_RETURN(result);
}

/* HRESULT IDirectSoundBuffer_SetFrequency(this, DWORD frequency); 0 = default */
HLE_EXPORT(IDirectSoundBuffer_SetFrequency)
{
    uint32_t iface = HLE_ARG(0), frequency = HLE_ARG(1), result = RECOMP_DSOUND_OK;
    uint64_t now = now_ms();
    Buffer *b;

    lock();
    b = model_for(iface, now);
    if (b) {
        pump(b, now);
        result = recomp_dsound_buffer_set_frequency(&b->model, frequency, now);
        if (result == RECOMP_DSOUND_OK)
            resync_output(b, 0);
    }
    unlock();
    HLE_RETURN(result);
}

/* void DirectSoundDoWork(void) -- the title's regular audio tick. The game's
 * own version services the emulated chip's deferred commands, which the
 * replaced buffers no longer need; here it feeds the host output. */
HLE_EXPORT(DirectSoundDoWork)
{
    uint64_t now = now_ms();
    uint32_t i;
    static int said;

    if (!said) {
        said = 1;
        fprintf(stderr, "[DSOUND] DirectSoundDoWork replaced by name: buffers "
                        "run on the host clock and play through XAudio2\n");
        fflush(stderr);
    }
    lock();
    for (i = 0; i < SLOTS; i++) {
        if (g_buffers[i].iface != 0u) {
            Buffer *b = model_for(g_buffers[i].iface, now);
            if (b)
                pump(b, now);
        }
    }
    unlock();
    HLE_RETURN(0);
}
