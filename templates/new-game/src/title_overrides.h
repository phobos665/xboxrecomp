/**
 * title_overrides.h - Per-title function override table
 *
 * Every override is a statement that the toolkit gets something wrong for one
 * specific game. Kept in engine code, those statements are indistinguishable
 * from general machinery, and the next title inherits behaviour nobody chose.
 * So they live in one per-title data file (title_overrides.c) and nowhere else.
 *
 * Every entry carries a reason. This is not documentation etiquette: an
 * override with no recorded reason cannot be re-evaluated later, because
 * nobody can tell whether the bug it worked around still exists. Undocumented
 * overrides are the main source of silent behavioural drift between titles.
 * RECOMP_OVERRIDE takes the reason as a required argument, and
 * recomp_overrides_init() refuses to start if one is empty.
 */

#ifndef TITLE_OVERRIDES_H
#define TITLE_OVERRIDES_H

#include <stddef.h>
#include <stdint.h>

typedef void (*recomp_func_t)(void);

typedef struct {
    uint32_t      xbox_va;  /* Xbox VA this override replaces */
    recomp_func_t handler;  /* Implementation to run instead */
    const char   *symbol;   /* Handler name, for logs (filled by the macro) */
    const char   *reason;   /* Why this override exists — mandatory */
} recomp_override_t;

/**
 * Declare one override.
 *
 * The reason is a required argument, so it cannot be forgotten by accident.
 * Write what the toolkit does wrong, not what the override does — "lifter
 * mis-translates the FPU spill at 0x12A40" rather than "fixes crash".
 *
 *   RECOMP_OVERRIDE(0x00012345, stub_matrix_setup,
 *                   "IDirect3DDevice8 vtable slot unresolved by func_id; "
 *                   "remove once vtable recovery seeds .rdata")
 */
#define RECOMP_OVERRIDE(va, fn, reason_text) \
    { (uint32_t)(va), (recomp_func_t)(fn), #fn, (reason_text) }

/* Defined in title_overrides.c — the per-title data file. */
extern const recomp_override_t g_title_overrides[];
extern const size_t            g_title_override_count;

/**
 * Validate and index the override table. Call once during startup, before any
 * game code runs. Aborts if an entry has an empty reason or a duplicate VA,
 * and logs every active override so the boot log records exactly which
 * per-title deviations are in force.
 */
void recomp_overrides_init(void);

/**
 * Look up an override for an Xbox VA, or NULL to fall through to the
 * auto-generated dispatch table.
 *
 * Called on every indirect call, so it is a binary search over a sorted index
 * and returns immediately when the table is empty.
 */
recomp_func_t recomp_lookup_manual(uint32_t xbox_va);

#endif /* TITLE_OVERRIDES_H */
