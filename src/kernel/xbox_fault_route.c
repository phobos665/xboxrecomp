/*
 * xbox_fault_route.c - which deliberate trap a host fault is.
 *
 * The route recomp_fault_install is given (recomp_fault.h). It used to be
 * route_device_fault plus two watchpoint calls in every title's main.c, as
 * Windows-only code; here it is once, for every host, in the order main.c
 * had it:
 *
 *   1. A single-step: a watchpoint stepping over the write it let through
 *      (Windows, and any host with a trap flag).
 *   2. An access to a watched page (RECOMP_WATCH_WRITE). Before the device
 *      ranges, because a watch is a deliberate trap the devices know nothing
 *      about.
 *   3. A trapped device range: the vblank interrupt page, the APU's
 *      registers, the AC'97 page -- whatever registered itself with
 *      recomp_fault_add_range.
 *   4. POSIX only: the rest of a 16 KB host page that a 4 KB trap made
 *      inaccessible. The guest page allows the access, so it is completed
 *      through the arena's backdoor as if nothing had trapped.
 *
 * Anything else is a crash, and the title's report gets it.
 */
#include "xbox_fault_route.h"

#include "platform/fault_emulate.h"
#include "xbox_memory_layout.h"
#include "xbox_watchpoint.h"

extern ptrdiff_t g_xbox_mem_offset;

int xbox_fault_route(recomp_fault *f)
{
    uintptr_t base = (uintptr_t)g_xbox_mem_offset;

    if (f->kind == RECOMP_FAULT_SINGLE_STEP)
        return xbox_watch_handle_trap(f);
    if (f->kind != RECOMP_FAULT_ACCESS)
        return 0;

    if (xbox_watch_handle_fault(f))
        return 1;

    if (!base || f->host_addr < base || f->host_addr - base > 0xFFFFFFFFu)
        return 0;
    if (recomp_fault_dispatch_ranges(f, (uint32_t)(f->host_addr - base)))
        return 1;

    return recomp_fault_passthrough(f);    /* 0 on Windows */
}
