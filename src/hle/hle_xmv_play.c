/*
 * hle_xmv_play.c -- see hle_xmv_play.h.
 *
 * Timing: a picture is shown when its timestamp comes due by the wall clock
 * from the first poll (movie_clock says why not the sound's position). Sound
 * is handed to the voice as the
 * demuxer reads it, a few hundred milliseconds ahead; a chunk the voice
 * refuses because its queue is full is offered again next poll, never
 * dropped (audio_output.h says why that matters).
 *
 * The picture goes to the host's movie layer (d3d8_movie), which the swap
 * draws while the title has its video overlay up. A title that draws the
 * movie surface as a texture instead would need the picture written into
 * that surface in its own format; the first poll logs the surface so that
 * can be added from evidence rather than guessed.
 */
#include "hle_xmv_play.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hle.h"
#include "../kernel/xbox_memory_layout.h"   /* xbox_EnvSwitch */

#if defined(_WIN32)
#include <windows.h>

#include "audio_output.h"
#include "xbox_adpcm.h"
#include "xmv_demux.h"
#include "xmv_decode.h"
#include "d3d8_movie.h"

/* kernel_path.c: an Xbox path to the host file it names. */
BOOL xbox_translate_path(const char *xbox_path, WCHAR *host_path_buf, DWORD buf_size);

#define MAX_MOVIES      4
#define MAX_CATCH_UP    8u          /* pictures decoded in one poll when behind */
#define AUDIO_CHUNK     8192u       /* bytes per submission */

typedef struct {
    int      used;
    xmv_demux dm;
    xmv_video_decoder *dec;
    uint8_t *bgra;
    uint32_t w, h;
    int      started;
    LARGE_INTEGER start;

    /* The next picture, read and not yet due. */
    int      have_next;
    uint8_t *next;
    uint32_t next_size, next_cap, next_pts;
    int      video_done;
    uint32_t shown;

    /* Sound: PCM waiting to be accepted by the voice, and ADPCM bytes that
     * do not yet make a whole block. */
    int      audio;               /* track 0 is played */
    uint32_t rate, channels, codec;
    uint8_t *pcm;
    uint32_t pcm_len, pcm_cap, pcm_off;
    uint8_t  adpcm_rest[72];
    uint32_t adpcm_rest_len;
    uint64_t pcm_submitted;
    int      logged_surface;
    uint32_t polls;
} movie;

static movie g_movies[MAX_MOVIES];
static LARGE_INTEGER g_qpf;

static movie *get(int handle)
{
    if (handle < 1 || handle > MAX_MOVIES || !g_movies[handle - 1].used)
        return NULL;
    return &g_movies[handle - 1];
}

/* The guest's path, ASCII or UTF-16, into `out`. 0 if it is neither. */
static int read_path(uint32_t va, char *out, size_t n)
{
    const uint8_t *p;
    size_t i;
    int wide;

    if (!va || n < 2)
        return 0;
    p = (const uint8_t *)HLE_PTR(va);
    wide = p[0] && p[1] == 0;
    for (i = 0; i + 1 < n; i++) {
        uint8_t c = wide ? p[i * 2] : p[i];
        if (wide && p[i * 2 + 1])
            return 0;
        if (!c)
            break;
        if (c < 0x20 || c > 0x7E)
            return 0;
        out[i] = (char)c;
    }
    out[i] = '\0';
    return i > 0;
}

static int append_pcm(movie *m, const uint8_t *data, uint32_t len)
{
    if (m->pcm_off && m->pcm_off == m->pcm_len)
        m->pcm_off = m->pcm_len = 0;
    if (m->pcm_len + len > m->pcm_cap) {
        uint32_t cap = (m->pcm_len + len) * 2u;
        uint8_t *q;
        if (m->pcm_off) {                    /* reclaim what has been played */
            memmove(m->pcm, m->pcm + m->pcm_off, m->pcm_len - m->pcm_off);
            m->pcm_len -= m->pcm_off;
            m->pcm_off = 0;
        }
        q = (uint8_t *)realloc(m->pcm, cap);
        if (!q)
            return 0;
        m->pcm = q;
        m->pcm_cap = cap;
    }
    memcpy(m->pcm + m->pcm_len, data, len);
    m->pcm_len += len;
    return 1;
}

/* Sound the demuxer has read so far, turned into PCM. */
static void take_audio(movie *m)
{
    const uint8_t *a;
    uint32_t n;

    if (!m->audio || !xmv_next_audio(&m->dm, 0, &a, &n))
        return;
    if (m->codec == 0x0001u) {
        append_pcm(m, a, n);
        return;
    }
    {
        /* Xbox ADPCM: 36-byte blocks per channel, 64 samples each. */
        uint32_t block = 36u * m->channels;
        int16_t out[64 * 2];

        while (n) {
            uint32_t take = block - m->adpcm_rest_len;
            if (take > n)
                take = n;
            memcpy(m->adpcm_rest + m->adpcm_rest_len, a, take);
            m->adpcm_rest_len += take;
            a += take;
            n -= take;
            if (m->adpcm_rest_len == block) {
                if (xbox_adpcm_decode_block(m->adpcm_rest, block, m->channels, out,
                                            64u * m->channels))
                    append_pcm(m, (const uint8_t *)out, 64u * m->channels * 2u);
                m->adpcm_rest_len = 0;
            }
        }
    }
}

static void feed_audio(movie *m)
{
    uint32_t frame = m->channels * 2u;

    while (m->pcm_off < m->pcm_len) {
        uint32_t n = m->pcm_len - m->pcm_off;
        if (n > AUDIO_CHUNK)
            n = AUDIO_CHUNK;
        n -= n % frame;
        if (!n || !recomp_audio_output_submit(RECOMP_AUDIO_SLOT_MOVIE, m->pcm + m->pcm_off, n,
                                              m->rate, m->channels, 16, 0))
            return;                      /* offered again next poll */
        m->pcm_off += n;
        m->pcm_submitted += n;
    }
}

/* Milliseconds into the movie, by the wall clock.
 *
 * Not the sound's position, though that is what a listener hears: the sound
 * for a packet is only read along with that packet's first picture, so a
 * clock that follows the sound stops when the sound runs out, the next
 * picture never comes due, and its sound is never read. Future Perfect's EA
 * logo stopped at exactly 1033 ms that way, with an empty voice. Started
 * together and played without gaps, the two stay together anyway. */
static uint32_t movie_clock(movie *m)
{
    LARGE_INTEGER now;

    QueryPerformanceCounter(&now);
    return (uint32_t)((now.QuadPart - m->start.QuadPart) * 1000 / g_qpf.QuadPart);
}

int xmv_play_open(uint32_t source_va, uint32_t *width, uint32_t *height)
{
    char path[300];
    WCHAR host[MAX_PATH * 2];
    FILE *f;
    movie *m = NULL;
    int i;

    if (!xbox_EnvSwitch("RECOMP_XMV_PLAY", 1))
        return 0;
    if (!read_path(source_va, path, sizeof path)) {
        fprintf(stderr, "[XMV] Create's source 0x%08X is not a path; the movie is skipped\n",
                source_va);
        return 0;
    }
    for (i = 0; i < MAX_MOVIES; i++)
        if (!g_movies[i].used) {
            m = &g_movies[i];
            break;
        }
    if (!m)
        return 0;
    if (!xbox_translate_path(path, host, MAX_PATH * 2) || !(f = _wfopen(host, L"rb"))) {
        fprintf(stderr, "[XMV] %s: not found on the host; the movie is skipped\n", path);
        return 0;
    }
    memset(m, 0, sizeof *m);
    if (xmv_open_file(&m->dm, f) != 0) {
        fprintf(stderr, "[XMV] %s: not an XMV file this reads; the movie is skipped\n", path);
        return 0;
    }
    m->dec = xmv_decoder_create(m->dm.width, m->dm.height, m->dm.extradata);
    if (!m->dec) {
        xmv_close(&m->dm);
        return 0;
    }
    m->w = m->dm.width;
    m->h = m->dm.height;
    m->bgra = (uint8_t *)calloc((size_t)m->w * m->h, 4);
    if (!m->bgra) {
        xmv_decoder_destroy(m->dec);
        xmv_close(&m->dm);
        return 0;
    }
    if (m->dm.audio_count > 0) {
        const xmv_audio_track *t = &m->dm.audio[0];
        if ((t->codec == 0x0001u && t->bits == 16) || t->codec == 0x0069u) {
            m->audio = t->channels == 1 || t->channels == 2;
            m->rate = t->rate;
            m->channels = t->channels;
            m->codec = t->codec;
        }
        if (!m->audio)
            fprintf(stderr, "[XMV] %s: sound codec 0x%04X is not played\n", path, t->codec);
    }
    if (!g_qpf.QuadPart)
        QueryPerformanceFrequency(&g_qpf);
    m->used = 1;
    *width = m->w;
    *height = m->h;
    fprintf(stderr, "[XMV] playing %s: %ux%u, %s\n", path, m->w, m->h,
            m->audio ? (m->codec == 0x69u ? "Xbox ADPCM sound" : "PCM sound") : "no sound");
    fflush(stderr);
    return (int)(m - g_movies) + 1;
}

void xmv_play_start(int handle)
{
    movie *m = get(handle);

    if (!m || m->started)
        return;
    m->started = 1;
    recomp_audio_output_reset_voice(RECOMP_AUDIO_SLOT_MOVIE);
    QueryPerformanceCounter(&m->start);
}

/* The next picture into m->next, if there is one. */
static int read_next(movie *m)
{
    const uint8_t *p;
    uint32_t n, pts;
    int key, r;

    if (m->have_next || m->video_done)
        return m->have_next;
    r = xmv_next_video(&m->dm, &p, &n, &pts, &key);
    take_audio(m);
    if (r != 1) {
        m->video_done = 1;
        return 0;
    }
    if (n > m->next_cap) {
        uint8_t *q = (uint8_t *)realloc(m->next, n);
        if (!q) {
            m->video_done = 1;
            return 0;
        }
        m->next = q;
        m->next_cap = n;
    }
    memcpy(m->next, p, n);
    m->next_size = n;
    m->next_pts = pts;
    m->have_next = 1;
    return 1;
}

static void log_surface(movie *m, uint32_t surface)
{
    if (m->logged_surface++ || !surface)
        return;
    fprintf(stderr, "[XMV] the title's movie surface 0x%08X: common 0x%08X data 0x%08X "
            "format 0x%08X size 0x%08X (not written: the picture is on the host's "
            "movie layer)\n", surface, HLE_MEM32(surface), HLE_MEM32(surface + 4),
            HLE_MEM32(surface + 12), HLE_MEM32(surface + 16));
    fflush(stderr);
}

uint32_t xmv_play_update(int handle, uint32_t surface_va)
{
    movie *m = get(handle);
    uint32_t now, decoded = 0;
    int got = 0;

    if (!m)
        return 2u;
    if (!m->started)
        xmv_play_start(handle);
    log_surface(m, surface_va);

    /* Keep the sound a little ahead: reading a picture reads its packet's
     * sound too, so reading ahead to the next picture is enough. */
    read_next(m);
    feed_audio(m);

    now = movie_clock(m);
    if (m->polls++ % 120u == 0u && m->polls < 120u * 20u) {
        uint64_t played = 0;
        uint32_t queued = 0;
        int pos = recomp_audio_output_position(RECOMP_AUDIO_SLOT_MOVIE, &played, &queued);
        fprintf(stderr, "[XMV] poll %u: clock %u ms, next picture %u ms%s, %u shown, "
                "sound %u bytes waiting, voice %s played %llu queued %u\n",
                m->polls, now, m->next_pts, m->video_done ? " (video done)" : "",
                m->shown, m->pcm_len - m->pcm_off, pos ? "position" : "no position",
                (unsigned long long)played, queued);
        fflush(stderr);
    }
    while (read_next(m) && m->next_pts <= now && decoded < MAX_CATCH_UP) {
        if (xmv_decoder_decode(m->dec, m->next, m->next_size, m->next_pts, m->bgra) == 1)
            got = 1;
        m->have_next = 0;
        decoded++;
    }
    feed_audio(m);
    if (got) {
        d3d8_movie_set_frame(m->bgra, m->w, m->h);
        m->shown++;
        return 1u;
    }
    if (m->video_done && !m->have_next) {
        /* Let the last of the sound finish before saying so. */
        uint64_t played = 0;
        uint32_t queued = 0;
        if (m->audio && m->pcm_off < m->pcm_len)
            return 0u;
        if (m->audio && recomp_audio_output_position(RECOMP_AUDIO_SLOT_MOVIE, &played, &queued) &&
            queued && now < m->next_pts + 2000u)
            return 0u;
        return 2u;
    }
    return 0u;
}

void xmv_play_close(int handle)
{
    movie *m = get(handle);

    if (!m)
        return;
    fprintf(stderr, "[XMV] movie closed after %u pictures\n", m->shown);
    fflush(stderr);
    recomp_audio_output_reset_voice(RECOMP_AUDIO_SLOT_MOVIE);
    d3d8_movie_clear();
    xmv_decoder_destroy(m->dec);
    xmv_close(&m->dm);
    free(m->bgra);
    free(m->next);
    free(m->pcm);
    memset(m, 0, sizeof *m);
}

#else  /* !_WIN32: no host player yet */

int xmv_play_open(uint32_t source_va, uint32_t *width, uint32_t *height)
{
    (void)source_va; (void)width; (void)height;
    return 0;
}
void xmv_play_start(int handle) { (void)handle; }
uint32_t xmv_play_update(int handle, uint32_t surface_va)
{
    (void)handle; (void)surface_va;
    return 2u;
}
void xmv_play_close(int handle) { (void)handle; }

#endif
