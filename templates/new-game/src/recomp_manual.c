/**
 * recomp_manual.c - Override dispatch and ICALL diagnostics
 *
 * ENGINE CODE. Nothing here is specific to any title, and nothing title-
 * specific should be added to it. Per-game overrides live in
 * title_overrides.c; see title_overrides.h for why the split exists.
 *
 * This file provides:
 *   - recomp_overrides_init()  : validate, sort and log the override table
 *   - recomp_lookup_manual()   : binary search, called on every indirect call
 *   - recomp_icall_fail_log()  : diagnostics when a target cannot be resolved
 *   - ICALL trace ring buffer  : globals written by the RECOMP_ICALL macro
 *
 * The recomp pipeline generates an auto-dispatch table (recomp_lookup) that
 * resolves most function addresses. recomp_lookup_manual() is consulted FIRST,
 * so an override always wins over generated code.
 */

#include "title_overrides.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/* ── ICALL trace ring buffer ───────────────────────────────── */

/*
 * Written by the RECOMP_ICALL macro (see recomp_types.h) on every indirect
 * call. After a crash, the VEH handler or recomp_icall_fail_log() dumps the
 * last 16 targets, which is usually enough to identify the call site.
 */
volatile uint32_t g_icall_trace[16]  = {0};
volatile uint32_t g_icall_trace_idx  = 0;
volatile uint64_t g_icall_count      = 0;

/* ── Sorted index over the override table ──────────────────── */

static const recomp_override_t **s_sorted;   /* ascending by xbox_va */
static size_t                    s_count;
static int                       s_initialized;

static int compare_by_va(const void *a, const void *b)
{
    uint32_t va_a = (*(const recomp_override_t * const *)a)->xbox_va;
    uint32_t va_b = (*(const recomp_override_t * const *)b)->xbox_va;

    if (va_a < va_b) return -1;
    if (va_a > va_b) return  1;
    return 0;
}

void recomp_overrides_init(void)
{
    size_t i;

    if (s_initialized)
        return;

    s_count = g_title_override_count;

    if (s_count == 0) {
        fprintf(stderr, "[OVERRIDE] no per-title overrides active\n");
        s_initialized = 1;
        return;
    }

    s_sorted = (const recomp_override_t **)
        malloc(s_count * sizeof(*s_sorted));
    if (!s_sorted) {
        fprintf(stderr, "[OVERRIDE] out of memory indexing %zu overrides\n",
                s_count);
        abort();
    }

    /*
     * Refuse to start on an entry with no reason. An override whose rationale
     * was never recorded cannot be re-evaluated later: nobody can tell whether
     * the bug it works around still exists, so it survives every cleanup and
     * quietly changes behaviour for the next title.
     */
    for (i = 0; i < s_count; i++) {
        const recomp_override_t *e = &g_title_overrides[i];

        if (!e->reason || !e->reason[0]) {
            fprintf(stderr,
                    "[OVERRIDE] entry %zu (VA 0x%08X -> %s) has no reason.\n"
                    "           Every override must record why it exists.\n",
                    i, e->xbox_va, e->symbol ? e->symbol : "?");
            abort();
        }
        if (!e->handler) {
            fprintf(stderr, "[OVERRIDE] entry %zu (VA 0x%08X) has no handler\n",
                    i, e->xbox_va);
            abort();
        }
        s_sorted[i] = e;
    }

    qsort(s_sorted, s_count, sizeof(*s_sorted), compare_by_va);

    /* Duplicate VAs make dispatch depend on table order. Reject them. */
    for (i = 1; i < s_count; i++) {
        if (s_sorted[i]->xbox_va == s_sorted[i - 1]->xbox_va) {
            fprintf(stderr,
                    "[OVERRIDE] duplicate override for VA 0x%08X (%s and %s)\n",
                    s_sorted[i]->xbox_va,
                    s_sorted[i - 1]->symbol, s_sorted[i]->symbol);
            abort();
        }
    }

    /*
     * Log the whole table at startup. Per-title deviations should be visible
     * in the boot log, not discovered later by reading source.
     */
    fprintf(stderr, "[OVERRIDE] %zu per-title override%s active:\n",
            s_count, s_count == 1 ? "" : "s");
    for (i = 0; i < s_count; i++) {
        fprintf(stderr, "  0x%08X -> %-28s %s\n",
                s_sorted[i]->xbox_va, s_sorted[i]->symbol, s_sorted[i]->reason);
    }
    fflush(stderr);

    s_initialized = 1;
}

/* ── Override lookup ───────────────────────────────────────── */

recomp_func_t recomp_lookup_manual(uint32_t xbox_va)
{
    size_t lo, hi;

    /*
     * Hot path: called on every indirect call. Most titles have no overrides
     * at all, so the empty case must cost a single compare.
     */
    if (s_count == 0)
        return (recomp_func_t)0;

    if (!s_initialized) {
        /*
         * Defensive: recomp_overrides_init() should run during startup, but a
         * lookup before it would otherwise read an unbuilt index.
         */
        static int warned;
        if (!warned) {
            warned = 1;
            fprintf(stderr, "[OVERRIDE] lookup before recomp_overrides_init(); "
                            "falling back to linear scan\n");
        }
        for (lo = 0; lo < g_title_override_count; lo++) {
            if (g_title_overrides[lo].xbox_va == xbox_va)
                return g_title_overrides[lo].handler;
        }
        return (recomp_func_t)0;
    }

    lo = 0;
    hi = s_count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        uint32_t va = s_sorted[mid]->xbox_va;

        if (va == xbox_va)
            return s_sorted[mid]->handler;
        if (va < xbox_va)
            lo = mid + 1;
        else
            hi = mid;
    }

    return (recomp_func_t)0;
}

/* ── ICALL failure logging ─────────────────────────────────── */

/*
 * Called when RECOMP_ICALL cannot resolve a target address. Usually one of:
 *   - A vtable dispatch to an address never detected as a function
 *   - A function pointer read from uninitialized or corrupt memory
 *   - A kernel thunk address the bridge does not handle
 *
 * Many of these are harmless during early bring-up: the ICALL macro pops the
 * dummy return address and continues. Investigate the ones that precede a
 * crash or visibly wrong behaviour.
 *
 * An unresolved target is usually a function-discovery gap rather than a
 * genuine per-title quirk. Prefer re-running discovery with --seed-functions
 * over adding an override, since a discovery fix helps every title and an
 * override helps exactly one.
 */
void recomp_icall_fail_log(uint32_t va)
{
    int i;

    fprintf(stderr, "[ICALL] Failed to resolve VA 0x%08X (total calls: %llu)\n",
            va, (unsigned long long)g_icall_count);

    fprintf(stderr, "  Recent ICALL targets:\n");
    for (i = 0; i < 16; i++) {
        int idx = (g_icall_trace_idx - 16 + i) & 15;
        if (g_icall_trace[idx])
            fprintf(stderr, "    [%2d] 0x%08X\n", i, g_icall_trace[idx]);
    }
    fflush(stderr);
}
