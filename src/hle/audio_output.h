/*
 * audio_output.h -- host audio output for replaced DirectSound buffers.
 *
 * From doaxbv-re (https://github.com/NoRain211/doaxbv-re,
 * recomp-runtime/audio_output.h), GPL-3.0. One behaviour change: output is on
 * at full gain unless RECOMP_AUDIO_GAIN says otherwise (doaxbv-re defaults to
 * muted while it validates its decoders).
 */
#ifndef XBOXRECOMP_AUDIO_OUTPUT_H
#define XBOXRECOMP_AUDIO_OUTPUT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Initialization is attempted once per process; an unavailable device or
   shutdown leaves guest audio timing unchanged. RECOMP_AUDIO_GAIN is the host
   master gain (0..1); absent means 1, zero mutes without opening a device. */
void recomp_audio_output_initialize(void);
void recomp_audio_output_shutdown(void);
void recomp_audio_output_reset_voice(uint32_t slot);

/* Copies PCM before returning; never retains a guest-memory pointer.
   Slots: 0..255. PCM: 1000..200000 Hz, mono/stereo, unsigned 8 or signed 16 bit.
   Each submission is frame-aligned and at most 160000 bytes. A full queue
   drops the incoming chunk without waiting. */
void recomp_audio_output_submit(
    uint32_t slot, const uint8_t *pcm, uint32_t bytes,
    uint32_t sample_rate, uint32_t channels, uint32_t bits_per_sample,
    int32_t volume_hundredth_db);

#ifdef __cplusplus
}
#endif

#endif
