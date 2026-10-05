/*
 * mod_patches.h -- tuning changes a mod makes to the title's own tables.
 *
 * A mod that rebalances a game -- a weapon's damage, a price, a timer --
 * usually wants to change a number in a table the title keeps in its XBE
 * image. Lifted code reads those tables from guest memory like the console
 * did, so changing the number in guest memory before the title starts is
 * the whole job. This does that from files, so a balance change is data
 * and not an override:
 *
 *   <mods>/patches/<anything>.json
 *
 *   {
 *     "title": "4553000A",                  optional: only for this title
 *     "patches": [
 *       { "address": "0x0025A0F0", "type": "f32",
 *         "expect": 1.0, "value": 1.5,
 *         "note": "what this is and how it was found" },
 *       { "address": "0x0025A100", "type": "u16",
 *         "expect": [10, 20, 30], "value": [12, 24, 36] },
 *       { "address": "0x0025A200", "type": "bytes",
 *         "expect": "00 01", "value": "01 01" }
 *     ]
 *   }
 *
 * Types: u8 u16 u32 i8 i16 i32 f32 f64, little-endian as the console stores
 * them, and "bytes" (a hex string). An array value writes consecutive
 * elements. "expect" is optional but strongly advised: a patch whose expect
 * does not match what is in memory is skipped and says why, so a patch
 * written for another release of the game does nothing instead of writing
 * over something else. "enabled": false switches one patch off.
 *
 * What this cannot do. It changes data, not behaviour: lifted code is
 * compiled C, so patching an instruction's bytes in .text changes nothing
 * (a patch into an executable section says so). A table the title builds
 * or loads from a file at run time is not there yet when patches apply;
 * replace the file through the mods folder instead.
 */
#ifndef XBOXRECOMP_MOD_PATCHES_H
#define XBOXRECOMP_MOD_PATCHES_H

#include <stddef.h>
#include <stdint.h>

/* Maps [va, va+len) to host memory, or NULL when any of it is not mapped. */
typedef uint8_t *(*mod_patch_guest_fn)(uint32_t va, uint32_t len, void *ctx);
/* Nonzero when va lies in a section the XBE marks executable; may be NULL. */
typedef int (*mod_patch_exec_fn)(uint32_t va, void *ctx);

typedef struct {
    int applied;    /* patches written */
    int skipped;    /* patches refused: expect mismatch, bad address, ... */
    int errors;     /* files or entries that could not be read */
} mod_patch_result;

/* Apply one patch file's text. `name` is for messages. `title_id` is the
 * running title (0 = unknown, which accepts any "title"). Adds to *result. */
void mod_patches_apply_text(const char *text, const char *name,
                            uint32_t title_id,
                            mod_patch_guest_fn guest, mod_patch_exec_fn exec,
                            void *ctx, mod_patch_result *result);

/* Apply every .json file in <mods>/patches, in name order, to guest memory.
 * Called once, after the XBE is mapped and the kernel bridge is built and
 * before any guest code runs. Silent when there is no mods folder or no
 * patches folder in it. */
void xbox_mod_patches_apply(void);

#endif /* XBOXRECOMP_MOD_PATCHES_H */
