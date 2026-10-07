/*
 * audio_output_clock.c -- the SDL3 host audio output keeps the contract the
 * DirectSound streams rely on (src/hle/audio_output.h), silently.
 *
 * RECOMP_MUTE=1 is forced, so nothing is ever heard. Checks:
 *   1. muted, the device still opens and a voice's play position advances
 *      at the device's rate (about real time), and stops when it runs dry;
 *   2. at most 12 submissions in flight: the 13th is refused, not dropped
 *      silently, and is taken again once the device has played some;
 *   3. reset_voice forgets the voice (position reports no voice).
 * Needs an audio device; prints SKIP and exits 0 when there is none.
 */
#include "audio_output.h"

#include <SDL3/SDL.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail;

#define CHECK(cond, ...) do { if (!(cond)) { printf("FAIL: " __VA_ARGS__); printf("\n"); g_fail++; } } while (0)

int main(void)
{
    enum { RATE = 22050, CHUNK = RATE / 10 * 4 };   /* 100 ms of 16-bit stereo */
    static uint8_t pcm[CHUNK];
    uint64_t played = 0, played2 = 0;
    uint32_t queued = 0;
    Uint64 t0, t1;
    int i, taken = 0;

    setenv("RECOMP_MUTE", "1", 1);
    unsetenv("RECOMP_AUDIO_GAIN");
    for (i = 0; i < CHUNK / 2; i++)                  /* a quiet square wave */
        ((int16_t *)pcm)[i] = (int16_t)(((i / 2) / 50) & 1 ? 800 : -800);

    /* The device opens on a host thread of its own, so the first submissions
     * are refused until it is up (and the caller, a guest thread in a title,
     * never waits for CoreAudio). */
    t0 = SDL_GetTicksNS();
    recomp_audio_output_initialize();
    {
        /* 0: neither the initialize nor a submission waits for the device
         * (a guest thread holding the guest lock makes them). */
        int took = recomp_audio_output_submit(256, pcm, CHUNK, RATE, 2, 16, 0);
        double ms = (double)(SDL_GetTicksNS() - t0) / 1e6;

        printf("initialize + first submission: %.1f ms (%s)\n", ms, took ? "taken" : "refused, device opening");
        CHECK(ms < 100.0, "initialize/submit blocked %.0f ms waiting for the device", ms);
        if (took)
            recomp_audio_output_reset_voice(256);
    }
    while (!recomp_audio_output_submit(256, pcm, CHUNK, RATE, 2, 16, 0)) {
        if (SDL_GetTicksNS() - t0 > 30000000000ull) {
            printf("%s: no audio device within 30 s (%s)\n", g_fail ? "FAIL" : "SKIP", SDL_GetError());
            return g_fail ? 1 : 0;
        }
        SDL_Delay(20);
    }
    printf("device up after %.0f ms\n", (double)(SDL_GetTicksNS() - t0) / 1e6);
    taken = 1;
    /* 2: fill the queue, then one more is refused. */
    while (taken < 12 && recomp_audio_output_submit(256, pcm, CHUNK, RATE, 2, 16, 0))
        taken++;
    CHECK(taken == 12, "only %d of 12 submissions taken", taken);
    CHECK(!recomp_audio_output_submit(256, pcm, CHUNK, RATE, 2, 16, 0),
          "a 13th submission was taken with 12 in flight");

    /* 1: the clock runs while muted. */
    CHECK(recomp_audio_output_position(256, &played, &queued), "no position for a live voice");
    t0 = SDL_GetTicksNS();
    SDL_Delay(500);
    recomp_audio_output_position(256, &played2, &queued);
    t1 = SDL_GetTicksNS();
    {
        double secs = (double)(t1 - t0) / 1e9;
        double rate = (double)(played2 - played) / 4.0 / secs;

        printf("muted: played %llu -> %llu bytes in %.3f s = %.0f frames/s (source %d), %u queued\n",
               (unsigned long long)played, (unsigned long long)played2, secs, rate, RATE, queued);
        CHECK(rate > RATE * 0.8 && rate < RATE * 1.2, "muted voice plays at %.0f frames/s, not ~%d",
              rate, RATE);
        CHECK(queued < 12, "nothing retired after 500 ms");
    }
    CHECK(recomp_audio_output_submit(256, pcm, CHUNK, RATE, 2, 16, 0),
          "a submission was refused after the device played some");

    /* It runs dry and stops: 13 x 100 ms queued in all. */
    SDL_Delay(1500);
    recomp_audio_output_position(256, &played, &queued);
    SDL_Delay(200);
    recomp_audio_output_position(256, &played2, &queued);
    printf("dry: %llu -> %llu bytes, %u queued\n", (unsigned long long)played,
           (unsigned long long)played2, queued);
    CHECK(played2 == played && played2 == 13ull * CHUNK, "position %llu after running dry, want %llu",
          (unsigned long long)played2, 13ull * CHUNK);
    CHECK(queued == 0, "%u still queued after running dry", queued);

    /* 3 */
    recomp_audio_output_reset_voice(256);
    CHECK(!recomp_audio_output_position(256, &played, &queued), "position after reset_voice");

    recomp_audio_output_shutdown();
    printf("%s\n", g_fail ? "FAIL" : "PASS");
    return g_fail ? 1 : 0;
}
