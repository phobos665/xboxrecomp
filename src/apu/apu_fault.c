/*
 * apu_fault.c - the trapped device pages, registered with the runtime's
 * fault route (platform/fault_emulate.h) instead of being named in every
 * title's main.c.
 *
 * Three ranges, each trapped by the memory layout under a switch:
 *
 *   0xFD600000  4 KB   NV2A PCRTC interrupt page, writes only (RECOMP_VBLANK):
 *                      PCRTC_INTR_0 is write-1-to-clear and PMC_INTR_0 bit 24
 *                      summarises it, so a vblank acknowledge can finish.
 *   0xFE800000  192 KB APU registers, reads and writes (RECOMP_AC97_READY):
 *                      served by the emulated APU.
 *   0xFEC00000  4 KB   AC'97 page, writes only (RECOMP_AC97_READY): the DSP
 *                      reset bit drops on the way in.
 *
 * On Windows each handler is the x86 decoder it always was
 * (apu_mmio_hook.c), called exactly as main.c's route_device_fault called it.
 * Elsewhere the faulting instruction is completed by
 * recomp_fault_emulate -- the host CPU's decoder -- against the same models,
 * with plain storage reached through recomp_guest_read/write, which never
 * fault even on the trapped page itself.
 *
 * A page larger than 4 KB on the host (16 KB on Apple Silicon) traps the
 * other 12 KB around each of the 4 KB pages too; those are not in these
 * ranges and the route's pass-through completes them as plain memory.
 */
#include "apu.h"
#include "platform/fault_emulate.h"

#include <stdint.h>
#include <stdio.h>

#if defined(_WIN32)
#include <windows.h>
#endif

#define GUEST_NV2A_BASE        0xFD000000u
#define GUEST_NV2A_PCRTC_PAGE  (GUEST_NV2A_BASE + XBOX_NV2A_PCRTC_PAGE)
#define GUEST_APU_REGS_BASE    0xFE800000u   /* PAGE_NOACCESS */
#define GUEST_APU_REGS_SIZE    0x00030000u   /* the DSP memory above stays RAM */
#define GUEST_AC97_PAGE        0xFEC00000u   /* PAGE_READONLY */

#define NV2A_PMC_INTR_0_OFF      0x000100u
#define NV2A_PMC_INTR_PCRTC_BIT  0x01000000u
#define NV2A_PCRTC_INTR_0_OFF    0x600100u

#if !defined(_WIN32)
/* ---- the device models, as callbacks over guest addresses ---------------- */

static uint64_t apu_rd(void *dev, uint32_t va, int size)
{
    return mcpx_apu_mmio_read((MCPXAPUState *)dev, va - GUEST_APU_REGS_BASE,
                              (unsigned)size);
}

static void apu_wr(void *dev, uint32_t va, uint64_t val, int size)
{
    mcpx_apu_mmio_write((MCPXAPUState *)dev, va - GUEST_APU_REGS_BASE, val,
                        (unsigned)size);
}

/* PCRTC_INTR_0 is write-1-to-clear and the PMC summary bit follows it; the
 * rest of the page is plain memory. Same model as nv2a_intr_handle_write. */
static void nv2a_intr_wr(void *dev, uint32_t va, uint64_t val, int size)
{
    (void)dev;
    if (va == GUEST_NV2A_BASE + NV2A_PCRTC_INTR_0_OFF) {
        uint32_t remaining = (uint32_t)recomp_guest_read(NULL, va, 4) & ~(uint32_t)val;
        uint32_t pmc = (uint32_t)recomp_guest_read(NULL, GUEST_NV2A_BASE + NV2A_PMC_INTR_0_OFF, 4);
        recomp_guest_write(NULL, va, remaining, 4);
        pmc = remaining ? (pmc | NV2A_PMC_INTR_PCRTC_BIT) : (pmc & ~NV2A_PMC_INTR_PCRTC_BIT);
        recomp_guest_write(NULL, GUEST_NV2A_BASE + NV2A_PMC_INTR_0_OFF, pmc, 4);
        return;
    }
    recomp_guest_write(NULL, va, val, size);
}

static void ac97_wr(void *dev, uint32_t va, uint64_t val, int size)
{
    (void)dev;
    recomp_guest_write(NULL, va,
                       mcpx_ac97_apply_mask(va - GUEST_APU_REGS_BASE, val), size);
}

static void say_undecoded(const char *what, recomp_fault *f, uint32_t va)
{
    static int said;
    char insn[64];
    if (said++ >= 20)
        return;
    recomp_fault_insn_text(f, insn, sizeof insn);
    fprintf(stderr, "[%s] undecoded access at 0x%08X, host insn %s\n", what, va, insn);
    fflush(stderr);
}
#endif

/* ---- the three range handlers -------------------------------------------- */

static int pcrtc_fault(recomp_fault *f, uint32_t va)
{
#if defined(_WIN32)
    return nv2a_intr_handle_write((struct _CONTEXT *)f->ctx, f->host_addr,
                                  va - GUEST_NV2A_BASE,
                                  recomp_fault_guest_base() + GUEST_NV2A_BASE);
#else
    if (recomp_fault_emulate(f, recomp_fault_guest_base(), NULL,
                             recomp_guest_read, nv2a_intr_wr))
        return 1;
    say_undecoded("NV2A", f, va);
    return 0;
#endif
}

static int apu_regs_fault(recomp_fault *f, uint32_t va)
{
#if defined(_WIN32)
    return apu_hook_handle_mmio((struct _CONTEXT *)f->ctx, f->host_addr, va,
                                f->is_write == 1);
#else
    if (!g_apu_state)
        return 0;     /* declined, as on Windows: the device does not exist */
    if (recomp_fault_emulate(f, recomp_fault_guest_base(), g_apu_state,
                             apu_rd, apu_wr))
        return 1;
    say_undecoded("APU", f, va);
    return 0;
#endif
}

static int ac97_fault(recomp_fault *f, uint32_t va)
{
#if defined(_WIN32)
    return mcpx_ac97_handle_write((struct _CONTEXT *)f->ctx, f->host_addr,
                                  va - GUEST_APU_REGS_BASE);
#else
    if (recomp_fault_emulate(f, recomp_fault_guest_base(), NULL,
                             recomp_guest_read, ac97_wr))
        return 1;
    say_undecoded("AC97", f, va);
    return 0;
#endif
}

void apu_fault_register(void)
{
    recomp_fault_add_range(GUEST_NV2A_PCRTC_PAGE, GUEST_NV2A_PCRTC_PAGE + 0x1000u,
                           1, pcrtc_fault, "NV2A PCRTC interrupt");
    recomp_fault_add_range(GUEST_APU_REGS_BASE, GUEST_APU_REGS_BASE + GUEST_APU_REGS_SIZE,
                           0, apu_regs_fault, "APU registers");
    recomp_fault_add_range(GUEST_AC97_PAGE, GUEST_AC97_PAGE + 0x1000u,
                           1, ac97_fault, "AC97");
}
