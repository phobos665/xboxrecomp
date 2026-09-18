/*
 * The one kernel symbol apu_dsp.c needs, stubbed for this test.
 *
 * This fork's apu_dsp.c finds the DSP doorbell by scanning the kernel's
 * contiguous blocks (upstream's copy is told the address instead), so the APU
 * library no longer links on its own. Linking xbox_kernel here would pull in
 * the video layer and the recompiled-code lookup for a test about mixing.
 *
 * "No contiguous blocks" is the honest answer for a unit test: doorbell
 * discovery then finds nothing, which is what this test wants -- it is about
 * the mixdown, and tests/apu_mixdown/README.md says what it measures.
 */
#include <stdint.h>

int xbox_ContiguousBlock(int index, uint32_t *addr, uint32_t *size)
{
    (void)index; (void)addr; (void)size;
    return 0;
}
