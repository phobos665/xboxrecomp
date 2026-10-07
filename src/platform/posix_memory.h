/*
 * posix_memory.h - the POSIX virtual-memory backend behind win32_compat.
 *
 * Internal to src/platform: win32_compat.c forwards the Win32 memory calls
 * here, and the few runtime pieces that need more than Win32 offers (the
 * layout reserving the guest span, a fault handler emulating a trapped
 * access) use the w32_* functions declared in win32_compat.h.
 *
 * What it has to provide, and why it is not a thin wrapper over mmap:
 *
 *  - Aliased views of one object (the RAM mirrors, the tiled aperture over
 *    the contiguous window). An unnamed shared-memory object: memfd on
 *    Linux, shm_open + immediate shm_unlink on Darwin.
 *
 *  - Windows' placement semantics. A fixed-address request either lands
 *    exactly there or fails; it never replaces what is already mapped. The
 *    layout depends on a failed placement failing. Inside the guest arena
 *    (one PROT_NONE reservation of the whole 4 GB guest span) MAP_FIXED is
 *    the only way to place anything, so the arena keeps its own record of
 *    what is placed and refuses an overlap itself.
 *
 *  - 4 KB protection on a host with bigger pages (16 KB on Apple Silicon).
 *    Every guest page's requested protection is kept in a side table. A
 *    host page gets the most restrictive protection of the guest pages on
 *    it, and an access the side table allows but the host page refuses is
 *    the fault handler's to complete, through the always-writable alias
 *    w32_backdoor() returns.
 */
#ifndef POSIX_MEMORY_H
#define POSIX_MEMORY_H

#if !defined(_WIN32)

#include "win32_compat.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct pm_object pm_object;

/* An unnamed shared-memory object of at least size bytes. NULL on failure. */
pm_object *pm_object_create(size_t size);
size_t     pm_object_size(const pm_object *o);
void       pm_object_retain(pm_object *o);
void       pm_object_release(pm_object *o);

/* A view of [off, off + len) of o. at == NULL lets the system choose; any
 * other address is honoured exactly or the call fails (Win32 semantics). */
void *pm_map_view(pm_object *o, size_t off, size_t len, int writable, void *at);
int   pm_unmap_view(const void *addr);

#ifdef __cplusplus
}
#endif

#endif /* !_WIN32 */
#endif /* POSIX_MEMORY_H */
