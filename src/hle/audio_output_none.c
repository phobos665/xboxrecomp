/*
 * audio_output_none.c -- no host audio output (hosts without XAudio2).
 *
 * Behaves exactly as audio_output_xaudio2.cpp does when it opens no device
 * (RECOMP_AUDIO_GAIN=0, or a failed XAudio2 start): nothing is taken and no
 * device clock exists, so a DirectSound stream falls back to its own clock
 * (audio_output.h). That is a configuration the Windows build already runs,
 * which is why this is the stand-in until an SDL3 output replaces it.
 */
#include "audio_output.h"

#include <stdio.h>

void recomp_audio_output_initialize(void)
{
    static int said;

    if (!said) {
        said = 1;
        fprintf(stderr, "[audio-output] no host audio output on this platform yet; "
                "playing nothing (streams use their own clock)\n");
    }
}

void recomp_audio_output_shutdown(void)
{
}

void recomp_audio_output_reset_voice(uint32_t slot)
{
    (void)slot;
}

int recomp_audio_output_submit(uint32_t slot, const uint8_t *pcm, uint32_t bytes,
                               uint32_t sample_rate, uint32_t channels,
                               uint32_t bits_per_sample, int32_t volume_hundredth_db)
{
    (void)slot; (void)pcm; (void)bytes; (void)sample_rate; (void)channels;
    (void)bits_per_sample; (void)volume_hundredth_db;
    return 0;
}

int recomp_audio_output_position(uint32_t slot, uint64_t *played_bytes,
                                 uint32_t *queued_buffers)
{
    (void)slot;
    if (played_bytes)
        *played_bytes = 0;
    if (queued_buffers)
        *queued_buffers = 0;
    return 0;
}
