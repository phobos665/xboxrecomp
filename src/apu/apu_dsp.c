/*
 * MCPX APU DSP (GP/EP) - Stub implementation
 *
 * The DSP Global Processor (GP) and Encode Processor (EP) handle effects
 * processing (reverb, chorus, etc.) and final output encoding. The full
 * DSP is ~3000 lines of DSP56300 emulation code.
 *
 * For initial audio, we bypass the DSP entirely:
 * - VP mixbins are passed directly to the EP output
 * - GP effects processing is skipped
 * - The EP just copies mixbin 0/1 (front L/R) to the monitor buffer
 *
 * This gives us basic voice playback without effects. The DSP can be
 * connected later for reverb, EQ, and other processing.
 *
 * Copyright (c) 2012 espes
 * Copyright (c) 2019-2025 Matt Borgerson
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 */

#include "apu_state.h"
#include "fpconv.h"

#include <stdlib.h>
#include <string.h>

/* ── DSP command doorbell acknowledgement ────────────────────────────────
 *
 * DirectSound does not stop at creating the device. It hands the audio DSP a
 * command block in guest RAM, writes a command word, and spins until the DSP
 * writes zero back. On real hardware the GP runs a DSP56300 program that does
 * that. Here the DSP is a passthrough stub, so the word never changes and the
 * title hangs inside DirectSound initialisation -- which on Wreckless gates the
 * entire engine, not just audio.
 *
 * RECOMP_APU_DSP_ACK=<addr>[,<addr>...] clears those guest dwords once per APU
 * frame, which is what "the command completed" looks like to the title.
 *
 * ponytail: this is a handshake acknowledgement, not a DSP. It says every
 * command succeeded instantly and computes nothing, so anything whose *result*
 * the title reads back will still be wrong. The real fix is DSP56300 emulation
 * in the GP/EP; this exists so audio init stops blocking everything behind it.
 *
 * The address is not derivable from the APU registers: GPSADDR/GPFADDR/
 * EPSADDR/EPFADDR point at the DSP's own scratch and frame memory, while the
 * command block is a DirectSound heap allocation. On Wreckless the registers
 * read 0x01504000 / 0x014EC000 / 0x0151C000 / 0x014F0000 and the doorbell is at
 * 0x014F8810 -- inside none of them. So it has to be observed: run with
 * RECOMP_WATCHDOG_SECS and the spin shows up as ebx plus the poll offset.
 */
#define APU_DSP_ACK_MAX 8
static uint32_t s_dsp_ack[APU_DSP_ACK_MAX];
static int s_dsp_ack_count = -1;

static void dsp_ack_init(void)
{
    const char *spec = getenv("RECOMP_APU_DSP_ACK");
    char buf[256], *p, *end;

    s_dsp_ack_count = 0;
    if (!spec || !*spec)
        return;
    strncpy(buf, spec, sizeof buf - 1);
    buf[sizeof buf - 1] = 0;
    for (p = buf; *p && s_dsp_ack_count < APU_DSP_ACK_MAX; ) {
        unsigned long v = strtoul(p, &end, 0);
        if (end == p)
            break;
        if (v)
            s_dsp_ack[s_dsp_ack_count++] = (uint32_t)v;
        p = (*end == ',') ? end + 1 : end;
    }
    if (s_dsp_ack_count)
        fprintf(stderr, "[APU] DSP doorbell ack: %d address(es), first 0x%08X\n",
                s_dsp_ack_count, s_dsp_ack[0]);
}

/* Find the doorbell rather than being told it.
 *
 * RECOMP_APU_DSP_ACK exists because the address is not derivable from the APU
 * registers, and naming it per title is the one genuinely per-game knob left
 * in the audio path. It does not have to be: the command block is a
 * contiguous allocation, the runtime now records every one it hands out, and
 * the doorbell sits at a fixed offset inside it -- Burnout 2's two are at
 * 0x80BB0810 and 0x80C18810, both `base + 0x810`.
 *
 * Scanning for a non-zero word at that offset is not enough on its own: a
 * framebuffer is a contiguous allocation too, and some pixel will hold a
 * small number. What separates a doorbell from data is that a stuck doorbell
 * does not change. The guest writes a command and spins, so the value stays
 * put; a pixel, a vertex buffer, an audio ring all move. So a candidate has
 * to hold the *same* non-zero value across several APU frames before it is
 * believed, and every address adopted is logged.
 *
 * Conservative on purpose. Writing four bytes into the wrong guest structure
 * is the kind of fault that surfaces somewhere else entirely, hours later.
 */
/* From the kernel's memory layout. Declared here rather than by including
 * kernel.h: the APU library does not depend on the kernel's headers, and this
 * is the whole of the interface it needs. */
int xbox_ContiguousBlock(int index, uint32_t *addr, uint32_t *size);

#define APU_DSP_DOORBELL_OFFSET  0x810u
#define APU_DSP_DOORBELL_STABLE  8        /* frames a value must persist */
#define APU_DSP_MAX_CANDIDATES   16

typedef struct {
    uint32_t addr;
    uint32_t value;
    uint32_t stable;
    int      adopted;
} DspDoorbell;

static DspDoorbell s_doorbell[APU_DSP_MAX_CANDIDATES];
static int s_doorbell_count;

static DspDoorbell *doorbell_slot(uint32_t addr)
{
    int i;

    for (i = 0; i < s_doorbell_count; i++)
        if (s_doorbell[i].addr == addr)
            return &s_doorbell[i];
    if (s_doorbell_count >= APU_DSP_MAX_CANDIDATES)
        return NULL;
    s_doorbell[s_doorbell_count].addr = addr;
    return &s_doorbell[s_doorbell_count++];
}

static void dsp_ack_discovered(MCPXAPUState *d)
{
    uint32_t addr, size;
    int index;

    for (index = 0; xbox_ContiguousBlock(index, &addr, &size); index++) {
        uint32_t slot_va = addr + APU_DSP_DOORBELL_OFFSET;
        volatile uint32_t *slot;
        DspDoorbell *cand;
        uint32_t v;

        if (size < APU_DSP_DOORBELL_OFFSET + 4)
            continue;

        slot = (volatile uint32_t *)(d->ram_ptr + slot_va);
        v = *slot;

        cand = doorbell_slot(slot_va);
        if (!cand)
            continue;

        if (cand->adopted) {
            if (v) {
                *slot = 0;
                cand->stable++;
            }
            continue;
        }

        /* A command is a small code -- Burnout 2 posts 2 and 3 -- and it has
         * to sit still. Anything large or moving is data. */
        if (!v || v > 0xFF) {
            cand->value = v;
            cand->stable = 0;
            continue;
        }
        if (v != cand->value) {
            cand->value = v;
            cand->stable = 1;
            continue;
        }
        if (++cand->stable < APU_DSP_DOORBELL_STABLE)
            continue;

        cand->adopted = 1;
        *slot = 0;
        fprintf(stderr, "[APU] DSP doorbell found at 0x%08X: command 0x%02X "
                        "held for %d frames, acknowledged\n",
                slot_va, v, APU_DSP_DOORBELL_STABLE);
        fflush(stderr);
    }
}

static void dsp_ack_frame(MCPXAPUState *d)
{
    int i;

    if (s_dsp_ack_count < 0)
        dsp_ack_init();
    if (!d->ram_ptr)
        return;

    /* An explicit list short-circuits the search: if a title has been
     * measured, there is no reason to re-derive it every frame. */
    if (s_dsp_ack_count) {
        for (i = 0; i < s_dsp_ack_count; i++) {
            uint32_t *slot = (uint32_t *)(d->ram_ptr + s_dsp_ack[i]);
            if (*slot) {
                static int shown[APU_DSP_ACK_MAX];
                if (shown[i]++ < 3)
                    fprintf(stderr, "[APU] DSP doorbell 0x%08X: command 0x%08X"
                                    " acknowledged\n", s_dsp_ack[i], *slot);
                *slot = 0;
            }
        }
        return;
    }

    /* Opt-in, because it does not work yet.
     *
     * The idea is sound and the mechanism is here: the command block is a
     * contiguous allocation, the runtime records every one, and a stuck
     * doorbell is the one word in such a block that holds a small value and
     * does not change. Measured on Burnout 2 it finds nothing -- the two
     * known doorbells at 0x80BB0810 and 0x80C18810 are not turning up at
     * `base + 0x810` of any tracked block, so either those blocks are not
     * allocated through xbox_ContiguousAlloc or the offset is not fixed.
     *
     * Left behind RECOMP_APU_DSP_SCAN rather than deleted: the next step is to
     * log the tracked blocks against the known addresses and see which
     * assumption is wrong. Enabled by default it would be a regression, since
     * with the explicit list the title reaches 1,345,011 guest calls and
     * without it 109,999.
     */
    if (getenv("RECOMP_APU_DSP_SCAN"))
        dsp_ack_discovered(d);
}

void mcpx_apu_dsp_init(MCPXAPUState *d)
{
    /* Allocate minimal DSP state for GP and EP.
     * We need these to exist so reset doesn't crash,
     * but they won't actually run DSP programs. */
    d->gp.dsp = (DSPState *)calloc(1, sizeof(DSPState));
    d->ep.dsp = (DSPState *)calloc(1, sizeof(DSPState));

    if (d->gp.dsp) d->gp.dsp->is_gp = true;
    if (d->ep.dsp) d->ep.dsp->is_gp = false;

    d->gp.realtime = false;
    d->ep.realtime = false;

    fprintf(stderr, "[APU] DSP GP/EP initialized (STUBBED - passthrough mode)\n");
}

void mcpx_apu_update_dsp_preference(MCPXAPUState *d)
{
    /* In the real xemu, this reads settings to decide whether
     * GP/EP should run in realtime or cached mode. We ignore it. */
    (void)d;
}

void mcpx_apu_dsp_frame(MCPXAPUState *d,
                         float mixbins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME])
{
    /* Bypass DSP: take mixbin 0 (front-left) and mixbin 1 (front-right)
     * and write them directly to the monitor frame buffer as the final
     * EP output.
     *
     * The Xbox DirectSound typically routes:
     *   Mixbin 0 = Front Left
     *   Mixbin 1 = Front Right
     *   Mixbin 2 = Center (often unused in stereo)
     *   Mixbin 3 = LFE
     *   Mixbin 4-5 = Rear L/R
     *
     * For stereo output, bins 0 and 1 are what we want.
     */

    int off = (d->ep_frame_div % 8) * NUM_SAMPLES_PER_FRAME;

    dsp_ack_frame(d);

    if (d->monitor.point != MCPX_APU_DEBUG_MON_VP) {
        for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++) {
            /* Clamp to [-1, 1] range */
            float left = mixbins[0][i];
            float right = mixbins[1][i];
            if (left > 1.0f) left = 1.0f;
            if (left < -1.0f) left = -1.0f;
            if (right > 1.0f) right = 1.0f;
            if (right < -1.0f) right = -1.0f;

            /* Convert to 16-bit and write (not accumulate) into frame buffer.
             * Each of the 8 sub-frames writes its own 32-sample slice. */
            d->monitor.frame_buf[off + i][0] = (int16_t)(left * 32767.0f);
            d->monitor.frame_buf[off + i][1] = (int16_t)(right * 32767.0f);
        }
    }

    g_dbg.gp.cycles = 0;
    g_dbg.ep.cycles = 0;
}
