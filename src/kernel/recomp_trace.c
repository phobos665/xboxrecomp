/**
 * Function tracing for recompiled code.
 *
 * recomp_types.h declares these and tools.recomp emits calls to them under
 * --trace-functions, but until now nothing defined them: the definitions lived
 * in one game project, so enabling tracing anywhere else failed at link time
 * with three unresolved symbols and no hint that the fix was to go and copy a
 * file. They belong with the runtime that declares them.
 *
 * Output goes to stderr, unbuffered, because the question tracing answers is
 * usually "what was the last thing that happened before it died".
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "xbox_memory_layout.h"

extern RECOMP_TLS uint32_t g_eax, g_ecx, g_edx, g_esp;
extern RECOMP_TLS uint32_t g_ebx, g_esi, g_edi;

/* A run that recurses produces trace lines without limit, and the useful
 * window is rarely the first few thousand. The budget stops a diagnostic from
 * filling a disk, and is deliberately generous: a budget that runs out before
 * the interesting part turns "no trace here" into a false negative, which is
 * worse than a large file. Override with RECOMP_TRACE_BUDGET. */
static long trace_budget(void)
{
    static long budget = -1;
    if (budget < 0) {
        const char *env = getenv("RECOMP_TRACE_BUDGET");
        budget = env ? strtol(env, NULL, 0) : 400000;
        if (budget < 0) budget = 0;
    }
    return budget > 0 ? budget-- : 0;
}

/* Where a title spends its calls.
 *
 * A recompiled title that is CPU-bound gives no clue which guest code is
 * responsible: the native profile is a wall of sub_XXXX, and the guest has no
 * program counter to sample. Counting entries does answer it, and the trace
 * hook is already on every function the run was generated to trace -- so
 * tracing everything and tallying instead of printing turns the existing
 * mechanism into a profile for the cost of an array increment.
 *
 * Counts, not time: a function called once that loops for a minute does not
 * appear here, and one called ten million times cheaply does. It says where
 * the calls go, which is the first question, not the last.
 *
 * Enable with RECOMP_TRACE_PROFILE=1. The report goes to stderr at exit,
 * hottest first.
 *
 * The stderr report shows the top 40 only, which answers "where do the calls
 * go" but cannot answer "did this particular function ever run" -- and that
 * second question is the one bring-up actually asks, because a function that
 * should have initialised something and did not is invisible in a top-40 list
 * of 25,000 entered functions. Reading absence out of that list is a mistake
 * that looks like evidence. Set RECOMP_PROFILE_DUMP to a path to get the
 * whole table instead, rewritten at every report so a run that faults still
 * leaves a complete one behind.
 *
 * The report only fires every N calls, so the table on disk lags the run by up
 * to N entries -- which is exactly the window a crash lands in. Reading "this
 * function is absent, so it never ran" off a lagging table is wrong in the one
 * case that matters. The crash handler calls this directly so the file covers
 * the fault itself; absence in a file written that way is real.
 */
#define PROF_SLOTS 65536                /* open addressing, power of two */
/* Sized for --trace-all-entries, which hooks every function rather than a
 * chosen few: a mid-size title lifts ~35,000, and a table that fills stops
 * counting exactly when the number is most interesting. It reports when it
 * is full, so a short count is never mistaken for a small frontier. */

static struct {
    uint32_t va;
    unsigned long long hits;
    unsigned long long first;   /* call ordinal of the first entry */
} g_prof[PROF_SLOTS];
static const char *g_prof_name[PROF_SLOTS];
static int g_prof_used, g_prof_full;
static unsigned long long g_prof_calls;

/* The complete set of functions entered, for "did X ever run".
 *
 * Rewritten from the start each time rather than appended: the file is then
 * always the current state, and a title that dies mid-run leaves a whole
 * table rather than a truncated stream. */
void recomp_profile_dump(void)
{
    const char *path = getenv("RECOMP_PROFILE_DUMP");
    FILE *f;
    int i;

    if (!path || !*path)
        return;
    f = fopen(path, "w");
    if (!f)
        return;
    fprintf(f, "# %d functions entered%s\n", g_prof_used,
            g_prof_full ? " (table full, some dropped)" : "");
    fprintf(f, "# va\tname\thits\tfirst_call\n");
    for (i = 0; i < PROF_SLOTS; i++) {
        if (!g_prof[i].hits)
            continue;
        fprintf(f, "0x%08X\t%s\t%llu\t%llu\n", g_prof[i].va,
                g_prof_name[i] ? g_prof_name[i] : "?", g_prof[i].hits,
                g_prof[i].first);
    }
    fclose(f);
}

static void prof_report(void)
{
    int taken[40], ntaken = 0;
    int i, j, shown;

    if (!g_prof_used)
        return;
    fprintf(stderr, "\n[PROFILE] %d functions entered%s, hottest first:\n",
            g_prof_used, g_prof_full ? " (table full, some dropped)" : "");
    for (shown = 0; shown < 40; shown++) {
        int best = -1;
        for (i = 0; i < PROF_SLOTS; i++) {
            if (!g_prof[i].hits)
                continue;
            for (j = 0; j < ntaken; j++)
                if (taken[j] == i)
                    break;
            if (j < ntaken)
                continue;
            if (best < 0 || g_prof[i].hits > g_prof[best].hits)
                best = i;
        }
        if (best < 0)
            break;
        taken[ntaken++] = best;
        fprintf(stderr, "  %14llu  %s (0x%08X)\n",
                g_prof[best].hits, g_prof_name[best] ? g_prof_name[best] : "?",
                g_prof[best].va);
    }
    fflush(stderr);
    recomp_profile_dump();
}

/* RECOMP_TRACE_PROFILE=1 profiles; a larger number is also how often to
 * report, in calls. The default suits a title burning a core in a spin loop;
 * a title that no longer has one may never reach it, and then the only report
 * is the one at exit -- which a killed run never gets. */
static unsigned long long prof_interval(void)
{
    static unsigned long long every;
    if (!every) {
        const char *v = getenv("RECOMP_TRACE_PROFILE");
        unsigned long long n = v ? strtoull(v, NULL, 0) : 0;
        every = n > 1 ? n : 20000000ull;
    }
    return every;
}

static int prof_enabled(void)
{
    static int on = -1;
    if (on < 0) {
        on = getenv("RECOMP_TRACE_PROFILE") ? 1 : 0;
        if (on)
            atexit(prof_report);
    }
    return on;
}

static void prof_count(const char *name, uint32_t va)
{
    unsigned i = (va * 2654435761u) & (PROF_SLOTS - 1);
    unsigned n;

    /* A title being profiled for a hang or a slowdown is a title that gets
     * killed rather than exited, and a kill does not reach atexit. Report as
     * it goes, so there is always a recent one. */
    if (++g_prof_calls % prof_interval() == 0)
        prof_report();

    for (n = 0; n < PROF_SLOTS; n++) {
        unsigned k = (i + n) & (PROF_SLOTS - 1);
        if (g_prof[k].va == va && g_prof[k].hits) { g_prof[k].hits++; return; }
        if (!g_prof[k].hits) {
            g_prof[k].va = va;
            g_prof[k].hits = 1;
            g_prof[k].first = g_prof_calls;
            g_prof_name[k] = name;
            g_prof_used++;
            return;
        }
    }
    g_prof_full = 1;
}


/* Watch one guest word and name whoever changes it.
 *
 * "This global is wrong by the time it is read" is the shape of most bring-up
 * faults, and reading the lifted code to find the writer does not scale: the
 * value can be written by a rep stos with a computed base, by a memcpy, or by
 * a function whose own bounds are wrong -- none of which mention the address.
 *
 * Sampling at function entry rather than trapping the write means the report
 * names the first function entered *after* the change, not the instruction
 * that made it. That is a coarser answer and a cheap one, and it is usually
 * enough: it narrows a 25,000-function run to a single call boundary.
 *
 * Set RECOMP_WATCH_VA to a guest address, e.g. RECOMP_WATCH_VA=0x5A8868.
 */
/* Readable through the guest mapping: plain RAM, the contiguous window, or a
 * device aperture. The apertures matter -- an interrupt-status register is
 * often the thing a stalled title is really waiting on, and rejecting it as
 * "out of range" hides that. */
static int guest_readable(uint32_t va, uint32_t bytes)
{
    if ((uint64_t)va + bytes <= (uint64_t)xbox_GetMappedSize())
        return 1;
    if (va >= 0x80000000u && (uint64_t)va + bytes <= 0x84000000ull)
        return 1;                            /* contiguous window */
    if (va >= 0xFD000000u && (uint64_t)va + bytes <= 0xFE000000ull)
        return 1;                            /* NV2A registers */
    return 0;
}

static uint32_t g_watch_va;
static uint32_t g_watch_last;
static int g_watch_on = -1, g_watch_primed;

static void watch_check(const char *name)
{
    const uint8_t *mem;
    uint32_t now;

    if (g_watch_on < 0) {
        const char *v = getenv("RECOMP_WATCH_VA");
        g_watch_va = v ? (uint32_t)strtoul(v, NULL, 0) : 0;
        g_watch_on = g_watch_va != 0;
    }
    if (!g_watch_on)
        return;
    if (!guest_readable(g_watch_va, 4))
        return;

    mem = (const uint8_t *)xbox_GetMemoryOffset();
    now = *(const uint32_t *)(mem + g_watch_va);
    if (!g_watch_primed) {
        g_watch_primed = 1;
        g_watch_last = now;
        fprintf(stderr, "[WATCH] 0x%08X starts as 0x%08X (at %s)\n",
                g_watch_va, now, name);
        return;
    }
    if (now != g_watch_last) {
        fprintf(stderr, "[WATCH] 0x%08X: 0x%08X -> 0x%08X, "
                "changed before entering %s (call %llu)\n",
                g_watch_va, g_watch_last, now, name, g_prof_calls);
        g_watch_last = now;
    }
}


/* Dump a run of guest words once, the first time any function is entered
 * after the address becomes readable.
 *
 * The reference oracle works by comparing structures, not single values: the
 * question is rarely "what is this word" and usually "does our copy of this
 * object look like the one xemu has". Reading it a word at a time through
 * RECOMP_WATCH_VA is too slow to do that, and the crash handler only dumps
 * when there is a crash.
 *
 * RECOMP_DUMP_VA=0x00F81000:16 prints sixteen words at that address.
 * Repeats are pointless -- the shape of a structure is what is being compared
 * -- so it fires once.
 */
/* Hold the dump until the nth function entry. */
#define return_if_early(count, spec) \
    do { if ((count) < strtoull((spec), NULL, 0)) return; } while (0)

static void dump_va_once(void)
{
    static int done;
    const char *spec;
    const uint8_t *mem;
    uint32_t va, n, i;
    char *colon;

    if (done)
        return;
    spec = getenv("RECOMP_DUMP_VA");
    if (!spec || !*spec)
        return;

    va = (uint32_t)strtoul(spec, &colon, 0);
    n = (colon && *colon == ':') ? (uint32_t)strtoul(colon + 1, NULL, 0) : 16;
    if (!n || n > 256)
        n = 16;

    if (!guest_readable(va, n * 4))
        return;                             /* not readable */

    mem = (const uint8_t *)xbox_GetMemoryOffset();

    /* Wait for the structure to exist.
     *
     * Firing on the first function entry dumps the address before anything
     * has written to it, and sixteen zero words then read as "ours is empty"
     * when the honest answer is "too early". Guest RAM starts zeroed, so a
     * range that is still entirely zero has not been built yet -- hold until
     * one word is not.
     *
     * A structure whose first bytes are legitimately zero is dumped as soon
     * as any later word is set, which is close enough; RECOMP_DUMP_AFTER
     * overrides for the case where the whole range really should be zero.
     */
    {
        static unsigned long long entries;
        static unsigned shown;
        const char *after = getenv("RECOMP_DUMP_AFTER");
        const char *every = getenv("RECOMP_DUMP_EVERY");
        uint32_t seen = 0;

        entries++;

        /* Sample repeatedly rather than once.
         *
         * A single sample has to be placed, and placing it means guessing the
         * entry rate: too early catches the structure half built, too late
         * never fires at all. Both happened here within one sitting.
         *
         * RECOMP_DUMP_EVERY=<n> prints every n entries instead, up to six
         * times. A field that is zero in all six, on a title that has stopped
         * changing, is genuinely never written -- which is the claim that
         * needed evidence. */
        if (every) {
            unsigned long long period = strtoull(every, NULL, 0);

            if (!period)
                period = 50000;
            if (entries % period || shown >= 6)
                return;
            shown++;
            mem = (const uint8_t *)xbox_GetMemoryOffset();
            fprintf(stderr, "[DUMP] %u words at 0x%08X (entry %llu):\n",
                    n, va, entries);
            for (i = 0; i < n; i++)
                fprintf(stderr, "  [0x%08X] = 0x%08X\n", va + i * 4,
                        *(const uint32_t *)(mem + va + i * 4));
            fflush(stderr);
            return;
        }

        /* Comparing a structure against a reference only means anything when
         * both are read at the same stage. Dumping at first-non-zero catches
         * ours part-built, and the fields the reference has set look missing
         * when they are merely later. RECOMP_DUMP_AFTER=<n> waits for the
         * nth function entry, so a stalled title can be sampled once it has
         * stopped changing. Its own counter, because g_prof_calls only moves
         * when profiling is on. */
        if (after)
            return_if_early(entries, after);

        for (i = 0; i < n; i++)
            seen |= *(const uint32_t *)(mem + va + i * 4);
        if (!seen)
            return;
    }

    done = 1;
    fprintf(stderr, "[DUMP] %u words at 0x%08X:\n", n, va);
    for (i = 0; i < n; i++)
        fprintf(stderr, "  [0x%08X] = 0x%08X\n", va + i * 4,
                *(const uint32_t *)(mem + va + i * 4));
    fflush(stderr);
}

/* RECOMP_TRACE_ONLY=<name>[,<name>...] -- print only these functions.
 *
 * The full trace prints every entry, which is millions of lines by the time a
 * title reaches anything interesting, and the budget that stops it filling a
 * disk runs out long before. This prints the named functions whatever the
 * budget, and dumps 24 words at ecx: for a thiscall method that is the object
 * itself, which is usually the state the question is about. Each name prints
 * at most RECOMP_TRACE_ONLY_MAX times (default 8).
 */
static int trace_only_match(const char *name)
{
    static int init;
    static char list[512];
    static int max_hits = 8;
    static int hits[16];
    const char *p;
    int idx = 0;
    size_t n = strlen(name);

    if (!init) {
        const char *v = getenv("RECOMP_TRACE_ONLY");
        const char *m = getenv("RECOMP_TRACE_ONLY_MAX");
        init = 1;
        if (v) {
            strncpy(list, v, sizeof list - 1);
            list[sizeof list - 1] = 0;
        }
        if (m)
            max_hits = atoi(m);
    }
    if (!list[0])
        return 0;
    for (p = list; *p; idx++) {
        const char *end = strchr(p, ',');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        if (len == n && strncmp(p, name, n) == 0) {
            if (idx < 16 && hits[idx] < max_hits) {
                hits[idx]++;
                return 1;
            }
            return 0;
        }
        if (!end)
            break;
        p = end + 1;
    }
    return 0;
}

static void trace_only_print(const char *name, uint32_t va)
{
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    uint32_t ret = guest_readable(g_esp, 4) ? *(const uint32_t *)(mem + g_esp) : 0;
    int i;

    fprintf(stderr, "[ONLY] -> %s (0x%08X) from=%08X esp=%08X eax=%08X "
            "ecx=%08X edx=%08X esi=%08X edi=%08X ebx=%08X\n",
            name, va, ret, g_esp, g_eax, g_ecx, g_edx, g_esi, g_edi, g_ebx);
    if (guest_readable(g_ecx, 96)) {
        for (i = 0; i < 24; i += 4) {
            const uint32_t *w = (const uint32_t *)(mem + g_ecx + i * 4);
            fprintf(stderr, "[ONLY]    [ecx+0x%02X] %08X %08X %08X %08X\n",
                    i * 4, w[0], w[1], w[2], w[3]);
        }
    }
    fflush(stderr);
}

void recomp_trace_enter(const char *name, uint32_t va)
{
    watch_check(name);
    dump_va_once();
    if (trace_only_match(name))
        trace_only_print(name, va);
    if (prof_enabled()) { prof_count(name, va); return; }
    if (!trace_budget()) return;
    /* The return address as well as the registers: at entry it is still at
     * [esp], and it names the call site, which is the thing a trace of "who
     * reached this" actually needs. Reading a guest stack dump for it works
     * only when the frames above are still live. */
    fprintf(stderr, "[TRACE] -> %s (0x%08X)  from=%08X esp=%08X eax=%08X "
            "ecx=%08X esi=%08X edi=%08X ebx=%08X\n",
            name, va,
            *(const uint32_t *)((uintptr_t)g_esp + xbox_GetMemoryOffset()),
            g_esp, g_eax, g_ecx, g_esi, g_edi, g_ebx);

    /* The stack arguments too, when asked. Registers alone do not say which
     * argument arrived null, and for a function with a long argument list,
     * counting pushes back from the call site is guesswork. */
    if (getenv("RECOMP_TRACE_ARGS")) {
        const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
        int n = atoi(getenv("RECOMP_TRACE_ARGS"));
        int i;

        if (n <= 0 || n > 32)
            n = 8;
        fprintf(stderr, "         args:");
        for (i = 1; i <= n; i++)
            fprintf(stderr, " %d=%08X", i,
                    *(const uint32_t *)(mem + g_esp + i * 4));
        fprintf(stderr, "\n");
        /* And the object eax points at. A matrix of NaNs says the maths went
         * wrong; whether its inputs were already zero says whether the maths
         * is at fault or the data behind it was never built. */
        /* Follow the pointer arguments one level. A matrix that arrives as
         * NaN was copied from somewhere, and the object it came from is what
         * needs looking at -- the value alone says only that it is wrong. */
        if (getenv("RECOMP_TRACE_DEREF")) {
            for (i = 1; i <= n; i++) {
                uint32_t a = *(const uint32_t *)(mem + g_esp + i * 4);
                int k;
                if (a < 0x00010000u || a >= 0x04000000u)
                    continue;
                fprintf(stderr, "         arg%d -> [%08X]:", i, a);
                for (k = 0; k < 12; k++)
                    fprintf(stderr, " %08X",
                            *(const uint32_t *)(mem + a + k * 4));
                fprintf(stderr, "\n");
            }
        }
        if (g_eax > 0x00010000u && g_eax < 0x04000000u) {
            fprintf(stderr, "         [eax=%08X]:", g_eax);
            for (i = 0; i < 24; i++)
                fprintf(stderr, " %08X",
                        *(const uint32_t *)(mem + g_eax + i * 4));
            fprintf(stderr, "\n");
        }
    }
    fflush(stderr);
}

/* Entry values answer "what was it called with"; only exit values answer "what
 * did the caller get back", which is the question when a callee-saved register
 * comes back wrong. */
void recomp_trace_exit(const char *name, uint32_t va)
{
    if (prof_enabled()) return;     /* entries alone carry the count */
    if (!trace_budget()) return;
    fprintf(stderr, "[TRACE] <- %s (0x%08X)  esp=%08X eax=%08X ecx=%08X "
            "esi=%08X edi=%08X ebx=%08X\n",
            name, va, g_esp, g_eax, g_ecx, g_esi, g_edi, g_ebx);
    fflush(stderr);
}

/* esp at a specific point inside a traced function. The epilogue's
 * `mov esp, ebp` hides drift from any return-time sample, so a leak of a few
 * bytes per call is only visible from a sample taken before it. */
void recomp_trace_esp(const char *name, const char *tag)
{
    if (prof_enabled()) return;
    if (!trace_budget()) return;
    fprintf(stderr, "[ESP] %s @%s  esp=%08X esi=%08X edi=%08X\n",
            name, tag, g_esp, g_esi, g_edi);
    fflush(stderr);
}

/* ---------------------------------------------------------------------------
 * Guest debug output (INT 2D / DebugService).
 *
 * The Xbox kernel debug trap. eax selects the service and ecx carries its
 * argument; service 1 is "print this ANSI_STRING", which is what
 * OutputDebugStringA and the XDK's DbgPrint compile down to. On hardware the
 * kernel consumes the trap and resumes at the int3 that follows, skipping it.
 *
 * Printing it is the whole point: this is the title telling us what it thinks
 * is happening, and during bring-up that is the most valuable output there is.
 * ------------------------------------------------------------------------- */

/* Defined in xbox_memory_layout.c; declared extern per consumer, as
 * kernel_bridge.c and nv2a_pb_replay.c already do. */
extern ptrdiff_t g_xbox_mem_offset;

void recomp_debug_service(uint32_t service, uint32_t arg_va)
{
    const uint8_t *mem = (const uint8_t *)g_xbox_mem_offset;
    uint16_t length;
    uint32_t buffer_va;

    if (service != 1) {
        fprintf(stderr, "[GUEST] DebugService %u (arg 0x%08X), ignored\n",
                (unsigned)service, arg_va);
        fflush(stderr);
        return;
    }

    /* ANSI_STRING { USHORT Length; USHORT MaximumLength; PCHAR Buffer; } */
    if (!arg_va)
        return;
    length    = *(const uint16_t *)(mem + arg_va);
    buffer_va = *(const uint32_t *)(mem + arg_va + 4);
    if (!buffer_va || !length)
        return;

    fprintf(stderr, "[GUEST] %.*s", (int)length, (const char *)(mem + buffer_va));
    if (length && ((const char *)(mem + buffer_va))[length - 1] != '\n')
        fputc('\n', stderr);
    fflush(stderr);
}
