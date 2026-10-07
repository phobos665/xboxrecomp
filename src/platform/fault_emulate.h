/*
 * fault_emulate.h - completing a trapped access, on any host.
 *
 * Some guest pages fault on purpose: device registers (the access has to
 * reach a device model), a watchpoint's page (the write has to be seen), and
 * on a host whose pages are bigger than the Xbox's 4 KB, the rest of a host
 * page that one 4 KB trap made inaccessible (the access is ordinary guest
 * memory and has to simply happen). In each case the handler performs the
 * access itself and steps the faulting thread past the instruction.
 *
 * What is the same everywhere is here: the table of trapped guest ranges and
 * who services each one, and recomp_fault_emulate, which decodes the
 * faulting instruction for the host CPU (x86-64: mmio_decode.h, arm64:
 * mmio_decode_a64.h) and runs it against a pair of callbacks. The route that
 * decides which case a fault is lives in the kernel (xbox_fault_route.c).
 */
#ifndef FAULT_EMULATE_H
#define FAULT_EMULATE_H

#include <stddef.h>
#include <stdint.h>

#include "recomp_fault.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef MMIO_ACCESS_FN_DEFINED
#define MMIO_ACCESS_FN_DEFINED
typedef uint64_t (*mmio_read_fn)(void *dev, uint32_t off, int size);
typedef void     (*mmio_write_fn)(void *dev, uint32_t off, uint64_t val, int size);
#endif

/* Where guest address 0 is on the host (g_xbox_mem_offset). The memory
 * layout sets it once it has mapped RAM; until then nothing here treats a
 * fault as a guest one. */
void      recomp_fault_set_guest_base(uintptr_t base);
uintptr_t recomp_fault_guest_base(void);

/* ---- trapped guest ranges ------------------------------------------------ */

/* Service a fault at guest address va. 1: done, resume. 0: not handled. */
typedef int (*recomp_range_fn)(recomp_fault *f, uint32_t va);

/* Register [lo_va, hi_va). write_only ranges are offered writes only (a
 * read-only trap page, where a read never faults on purpose). Called during
 * start-up, before the pages are trapped; at most 16. Returns 0 on success. */
int recomp_fault_add_range(uint32_t lo_va, uint32_t hi_va, int write_only,
                           recomp_range_fn fn, const char *name);

/* Offer a fault at guest va to the range that covers it. Safe in a signal
 * handler: the table is read without a lock. */
int recomp_fault_dispatch_ranges(recomp_fault *f, uint32_t va);

/* ---- completing the access ----------------------------------------------- */

/* Run the faulting instruction against rd/wr, with offsets taken from
 * dev_base (the host address of offset 0), write the resulting registers back
 * into f->ctx and step past it. 1 on success; 0 for an instruction the host's
 * decoder does not know, with nothing changed. On x86-64 the decoder sees
 * only the faulting address, so the offset is f->host_addr - dev_base; on
 * arm64 every element's own address is computed from the registers. */
int recomp_fault_emulate(recomp_fault *f, uintptr_t dev_base, void *dev,
                         mmio_read_fn rd, mmio_write_fn wr);

/* For a guest-memory access (POSIX guest arena only): may this element go
 * ahead? host is the element's host address. */
typedef int (*recomp_allow_fn)(uintptr_t host, int size, int is_write, void *arg);

/* Complete a guest-memory access through the arena's always-writable alias
 * (w32_backdoor), the page's own protection left exactly as it was -- so no
 * other thread ever sees it open. Every element is checked with allow first,
 * on a dry run, and nothing is done unless all pass. POSIX only; 0 on
 * Windows. */
int recomp_fault_complete_guest(recomp_fault *f, recomp_allow_fn allow, void *arg);

/* The 16 KB-page case: an access the 4 KB guest page allows, refused only
 * because another 4 KB page on the same host page is trapped. Completes it
 * and returns 1; 0 if it is not that case. POSIX only. */
int recomp_fault_passthrough(recomp_fault *f);

/* Guest-memory reads and writes that never fault: through the arena's
 * backdoor on POSIX, direct on Windows. For a fault handler that has to read
 * or write a trapped page (a device model's store, a watch's before/after).
 * dev is ignored; off is the host address minus the arena base, i.e. the
 * guest VA. */
uint64_t recomp_guest_read(void *dev, uint32_t va, int size);
void     recomp_guest_write(void *dev, uint32_t va, uint64_t val, int size);

/* Per-thread set-up of the thread-locals this uses inside a fault handler
 * (recomp_fault_thread_init calls it). Nothing on Windows. */
void recomp_fault_emulate_thread_init(void);

/* The host instruction at the fault, as text, for a report: x86 bytes, or
 * the arm64 word. */
void recomp_fault_insn_text(const recomp_fault *f, char *buf, size_t n);

#ifdef __cplusplus
}
#endif

#endif /* FAULT_EMULATE_H */
