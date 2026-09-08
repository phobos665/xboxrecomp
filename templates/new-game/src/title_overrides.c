/**
 * title_overrides.c - PER-TITLE DATA. Not engine code.
 *
 * This is the one file where per-game hacks belong. Everything here describes
 * this title only and must not be copied to another game without re-deriving
 * it: the addresses are specific to one XBE, and the workarounds are specific
 * to bugs that may since have been fixed properly.
 *
 * If you find yourself wanting to edit recomp_manual.c instead, that is a sign
 * the fix belongs in the toolkit, not in a title.
 *
 * Every entry needs a reason. See title_overrides.h.
 */

#include "title_overrides.h"

#include <stdio.h>

/* Register state from the generated code, for handlers that need it. */
extern uint32_t g_eax, g_ebx, g_ecx, g_edx, g_esp, g_ebp, g_esi, g_edi;

/* ── Handlers ───────────────────────────────────────────────
 *
 * Write the replacement implementations here, above the table.
 *
 * Common shapes:
 *
 *   // Stub: skip a function entirely, returning 0 in eax.
 *   static void stub_00067890(void) {
 *       g_eax = 0;
 *   }
 *
 *   // Trace: wrap the generated function to log entry and exit.
 *   extern void sub_00012345(void);
 *   static void traced_sub_00012345(void) {
 *       fprintf(stderr, "[TRACE] enter 0x00012345 ecx=0x%08X\n", g_ecx);
 *       sub_00012345();
 *       fprintf(stderr, "[TRACE] leave 0x00012345 eax=0x%08X\n", g_eax);
 *   }
 *
 *   // Fix: hand-written replacement for a mis-translated function.
 *   // Read arguments per the guest calling convention: __stdcall and
 *   // __cdecl take them off the simulated stack, __fastcall in ecx/edx.
 *   static void fixed_sub_000ABCDE(void) {
 *       uint32_t this_ptr = g_ecx;
 *       uint32_t arg1     = MEM32(g_esp + 4);
 *       (void)this_ptr; (void)arg1;
 *       g_eax = 0;
 *   }
 */

/* ── Override table ─────────────────────────────────────────
 *
 * Entries may be listed in any order; recomp_overrides_init() sorts them.
 *
 * Each reason should say what the toolkit gets wrong and what would let the
 * override be deleted. A reason that only describes the symptom ("fixes
 * crash") cannot be re-evaluated later and will outlive the bug it works
 * around.
 */
const recomp_override_t g_title_overrides[] = {
    /*
     * Example entries — delete these and add your own.
     *
     * RECOMP_OVERRIDE(0x00012345, traced_sub_00012345,
     *     "Tracing call flow into the render loop while bringing up D3D; "
     *     "temporary, remove before shipping"),
     *
     * RECOMP_OVERRIDE(0x00067890, stub_00067890,
     *     "Spins on an APU DMA completion bit the audio HLE never sets; "
     *     "remove once apu_vp reports buffer completion"),
     *
     * RECOMP_OVERRIDE(0x000ABCDE, fixed_sub_000ABCDE,
     *     "Lifter drops the x87 spill across the call at 0xABD02, so the "
     *     "returned matrix is garbage; remove when the FP stack model lands"),
     */

    /* Keep at least one element so the array is valid C when empty. */
    { 0, 0, 0, 0 }
};

/*
 * Count excludes the trailing sentinel above.
 */
const size_t g_title_override_count =
    (sizeof(g_title_overrides) / sizeof(g_title_overrides[0])) - 1;
