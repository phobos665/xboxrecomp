/*
 * mmio_decode_a64.h -- complete one trapped AArch64 load or store.
 *
 * The arm64 counterpart of mmio_decode.h. A page that has to fault -- a
 * device register page, a watched page, or a 4 KB page whose 16 KB host page
 * another 4 KB page made read-only -- traps every access to it, and the
 * handler then has to do what the instruction would have done: perform the
 * access against something else (a device model, or an always-writable alias
 * of the same memory), write the result into the destination register,
 * apply any base-register writeback, and step the PC over it.
 *
 * Unlike the x86 decoder, this one is not limited to the forms device code
 * uses. On a 16 KB-page host the collateral 12 KB of a trapped page is
 * ordinary guest memory, reached by whatever clang emitted for the lifted
 * code and the runtime -- every load/store addressing mode, FP/SIMD
 * registers up to Q (inlined memcpy), pairs, and the LSE atomics and
 * exclusives the Interlocked* primitives compile to. What clang emits for
 * the lifted MEM8..MEM64 accesses was measured on a TimeSplitters 2 chunk:
 * LDR/STR with a register offset (`[x, w, uxtw]`, the XBOX_PTR form) or an
 * immediate, LDUR/STUR, pre/post-index, LDRB/LDRH/LDRSB/LDRSH/LDRSW, FP S/D
 * and Q loads and stores, and LDP/STP.
 *
 * Anything else returns 0 and the fault goes through as a crash: stepping
 * over an instruction that was not understood would corrupt the guest
 * silently.
 *
 * Pure and header-only: the decoder works on a plain register file, so it
 * can be tested on encoded words with no fault and no device, and the
 * signal handler copies ucontext in and out (mmio_a64_from_ucontext).
 */
#ifndef MMIO_DECODE_A64_H
#define MMIO_DECODE_A64_H

#include <stdint.h>
#include <string.h>

/* Same shape as mmio_decode.h's, so a device's two callbacks serve both. */
#ifndef MMIO_ACCESS_FN_DEFINED
#define MMIO_ACCESS_FN_DEFINED
typedef uint64_t (*mmio_read_fn)(void *dev, uint32_t off, int size);
typedef void     (*mmio_write_fn)(void *dev, uint32_t off, uint64_t val, int size);
#endif

/* Atomics, for a caller whose target is real shared memory (the guest
 * arena's backdoor) and so can do them for real. Without one, an atomic is
 * completed as a read and a write, which is right for a device model and
 * not atomic against another thread. op is one of MMIO_AT_*; the result is
 * the old value (for MMIO_AT_LDX the value loaded). MMIO_AT_CAS stores
 * operand if the old value equals expected. MMIO_AT_STX stores operand if
 * nothing has changed the location since this thread's MMIO_AT_LDX, and
 * sets *status to 0 on success, 1 on failure -- STXR's own result. */
enum {
    MMIO_AT_ADD, MMIO_AT_CLR, MMIO_AT_EOR, MMIO_AT_SET,
    MMIO_AT_SMAX, MMIO_AT_SMIN, MMIO_AT_UMAX, MMIO_AT_UMIN,   /* = LSE opc */
    MMIO_AT_SWP, MMIO_AT_CAS, MMIO_AT_LDX, MMIO_AT_STX
};
typedef uint64_t (*mmio_atomic_fn)(void *dev, uint32_t off, int size, int op,
                                   uint64_t operand, uint64_t expected, int *status);

typedef struct mmio_a64_ctx {
    uint64_t x[31];        /* x0..x30 (x29 fp, x30 lr) */
    uint64_t sp;
    uint64_t pc;
    uint64_t v[32][2];     /* SIMD/FP v0..v31, low half first */
} mmio_a64_ctx;

/* Register n of a load/store's Rt/Rm field: 31 is the zero register. */
static inline uint64_t mmio_a64_xr(const mmio_a64_ctx *c, unsigned n)
{
    return n == 31 ? 0 : c->x[n];
}

static inline void mmio_a64_xw(mmio_a64_ctx *c, unsigned n, uint64_t v)
{
    if (n != 31)
        c->x[n] = v;
}

/* Rn, the base: 31 is SP. */
static inline uint64_t mmio_a64_base(const mmio_a64_ctx *c, unsigned n)
{
    return n == 31 ? c->sp : c->x[n];
}

static inline void mmio_a64_base_w(mmio_a64_ctx *c, unsigned n, uint64_t v)
{
    if (n == 31) c->sp = v; else c->x[n] = v;
}

static inline int64_t mmio_a64_sext(uint64_t v, int bits)
{
    uint64_t m = 1ULL << (bits - 1);
    v &= (bits == 64) ? ~0ULL : ((1ULL << bits) - 1);
    return (int64_t)((v ^ m) - m);
}

/* One element of an access. size is 1, 2, 4, 8 or 16; a 16-byte element is
 * two 8-byte callbacks, low half first. */
static inline void mmio_a64_read(uintptr_t ea, uintptr_t dev_base, void *dev,
                                 mmio_read_fn rd, int size, uint64_t out[2])
{
    uint32_t off = (uint32_t)(ea - dev_base);
    out[1] = 0;
    if (size == 16) {
        out[0] = rd(dev, off, 8);
        out[1] = rd(dev, off + 8, 8);
    } else {
        out[0] = rd(dev, off, size);
        if (size < 8)
            out[0] &= (1ULL << (size * 8)) - 1;
    }
}

static inline void mmio_a64_write(uintptr_t ea, uintptr_t dev_base, void *dev,
                                  mmio_write_fn wr, int size, const uint64_t in[2])
{
    uint32_t off = (uint32_t)(ea - dev_base);
    if (size == 16) {
        wr(dev, off, in[0], 8);
        wr(dev, off + 8, in[1], 8);
    } else {
        uint64_t v = size < 8 ? in[0] & ((1ULL << (size * 8)) - 1) : in[0];
        wr(dev, off, v, size);
    }
}

/* The data of FP/SIMD register n, for a store of `size` bytes. */
static inline void mmio_a64_vget(const mmio_a64_ctx *c, unsigned n, int size, uint64_t out[2])
{
    out[0] = c->v[n][0];
    out[1] = size == 16 ? c->v[n][1] : 0;
    if (size < 8)
        out[0] &= (1ULL << (size * 8)) - 1;
}

/* A scalar FP/SIMD load writes the element and zeroes the rest of the
 * register. */
static inline void mmio_a64_vset(mmio_a64_ctx *c, unsigned n, const uint64_t in[2])
{
    c->v[n][0] = in[0];
    c->v[n][1] = in[1];
}

/* A loaded integer into Rt: sext says sign-extend from size bytes, and
 * to64 whether Rt is an X register (else a W register, upper half zero). */
static inline void mmio_a64_set_loaded(mmio_a64_ctx *c, unsigned rt, uint64_t v,
                                       int size, int sext, int to64)
{
    if (sext)
        v = (uint64_t)mmio_a64_sext(v, size * 8);
    if (!to64)
        v &= 0xFFFFFFFFULL;
    mmio_a64_xw(c, rt, v);
}

/* Integer load/store (single register): opc and size as encoded. */
static inline int mmio_a64_int_access(mmio_a64_ctx *c, uintptr_t ea, unsigned size_f,
                                      unsigned opc, unsigned rt, uintptr_t dev_base,
                                      void *dev, mmio_read_fn rd, mmio_write_fn wr)
{
    int size = 1 << size_f;
    uint64_t d[2] = {0, 0};

    switch (opc) {
    case 0:                                         /* STR*  */
        d[0] = mmio_a64_xr(c, rt);
        mmio_a64_write(ea, dev_base, dev, wr, size, d);
        return 1;
    case 1:                                         /* LDR*, zero-extending */
        mmio_a64_read(ea, dev_base, dev, rd, size, d);
        mmio_a64_set_loaded(c, rt, d[0], size, 0, size == 8);
        return 1;
    case 2:                                         /* LDRS* to X, or PRFM */
        if (size_f == 3)
            return 1;                               /* PRFM: no access */
        mmio_a64_read(ea, dev_base, dev, rd, size, d);
        mmio_a64_set_loaded(c, rt, d[0], size, 1, 1);
        return 1;
    case 3:                                         /* LDRSB/LDRSH to W */
        if (size_f >= 2)
            return 0;                               /* unallocated */
        mmio_a64_read(ea, dev_base, dev, rd, size, d);
        mmio_a64_set_loaded(c, rt, d[0], size, 1, 0);
        return 1;
    }
    return 0;
}

/* FP/SIMD load/store (single register). */
static inline int mmio_a64_fp_access(mmio_a64_ctx *c, uintptr_t ea, unsigned size_f,
                                     unsigned opc, unsigned rt, uintptr_t dev_base,
                                     void *dev, mmio_read_fn rd, mmio_write_fn wr)
{
    int size;
    uint64_t d[2] = {0, 0};

    if (opc & 2) {
        if (size_f != 0)
            return 0;                               /* unallocated */
        size = 16;
    } else {
        size = 1 << size_f;
    }
    if (opc & 1) {
        mmio_a64_read(ea, dev_base, dev, rd, size, d);
        mmio_a64_vset(c, rt, d);
    } else {
        mmio_a64_vget(c, rt, size, d);
        mmio_a64_write(ea, dev_base, dev, wr, size, d);
    }
    return 1;
}

/* Bytes an FP/SIMD single-register access moves, for immediate scaling. */
static inline unsigned mmio_a64_fp_scale(unsigned size_f, unsigned opc)
{
    return (opc & 2) ? 4 : size_f;
}

/*
 * Complete the load/store `insn` at c->pc. Offsets passed to rd/wr are
 * effective address minus dev_base. Returns 1 with registers updated and
 * c->pc advanced, or 0 for an instruction this does not handle.
 */
static inline int mmio_a64_emulate_ex(mmio_a64_ctx *c, uint32_t insn, uintptr_t dev_base,
                                      void *dev, mmio_read_fn rd, mmio_write_fn wr,
                                      mmio_atomic_fn at)
{
    unsigned rt = insn & 31, rn = (insn >> 5) & 31;
    unsigned size_f = insn >> 30;
    unsigned v = (insn >> 26) & 1;

    if (!rd || !wr)
        return 0;

    /* Load/store register, unsigned immediate:
     *   size 111 V 01 opc imm12 Rn Rt */
    if ((insn & 0x3B000000u) == 0x39000000u) {
        unsigned opc = (insn >> 22) & 3;
        unsigned scale = v ? mmio_a64_fp_scale(size_f, opc) : size_f;
        uintptr_t ea = (uintptr_t)(mmio_a64_base(c, rn)
                                   + ((uint64_t)((insn >> 10) & 0xFFF) << scale));
        int ok = v ? mmio_a64_fp_access(c, ea, size_f, opc, rt, dev_base, dev, rd, wr)
                   : mmio_a64_int_access(c, ea, size_f, opc, rt, dev_base, dev, rd, wr);
        if (ok) c->pc += 4;
        return ok;
    }

    /* Load/store register, immediate pre/post-index, unscaled (LDUR/STUR)
     * and unprivileged (LDTR/STTR):
     *   size 111 V 00 opc 0 imm9 op2 Rn Rt */
    if ((insn & 0x3B200000u) == 0x38000000u) {
        unsigned opc = (insn >> 22) & 3, op2 = (insn >> 10) & 3;
        int64_t imm = mmio_a64_sext((insn >> 12) & 0x1FF, 9);
        uint64_t base = mmio_a64_base(c, rn);
        uintptr_t ea = (uintptr_t)(op2 == 1 ? base : base + (uint64_t)imm);
        int ok;

        if (v && op2 == 2)
            return 0;                               /* unallocated */
        ok = v ? mmio_a64_fp_access(c, ea, size_f, opc, rt, dev_base, dev, rd, wr)
               : mmio_a64_int_access(c, ea, size_f, opc, rt, dev_base, dev, rd, wr);
        if (!ok)
            return 0;
        if (op2 == 1 || op2 == 3)                   /* post / pre: write back */
            mmio_a64_base_w(c, rn, base + (uint64_t)imm);
        c->pc += 4;
        return 1;
    }

    /* Load/store register, register offset:
     *   size 111 V 00 opc 1 Rm option S 10 Rn Rt */
    if ((insn & 0x3B200C00u) == 0x38200800u) {
        unsigned opc = (insn >> 22) & 3, rm = (insn >> 16) & 31;
        unsigned option = (insn >> 13) & 7, s = (insn >> 12) & 1;
        unsigned scale = v ? mmio_a64_fp_scale(size_f, opc) : size_f;
        uint64_t off = mmio_a64_xr(c, rm);
        uintptr_t ea;
        int ok;

        switch (option) {
        case 2: off = (uint32_t)off; break;                         /* UXTW */
        case 3: break;                                              /* LSL/UXTX */
        case 6: off = (uint64_t)(int64_t)(int32_t)(uint32_t)off; break; /* SXTW */
        case 7: break;                                              /* SXTX */
        default: return 0;
        }
        if (s) off <<= scale;
        ea = (uintptr_t)(mmio_a64_base(c, rn) + off);
        ok = v ? mmio_a64_fp_access(c, ea, size_f, opc, rt, dev_base, dev, rd, wr)
               : mmio_a64_int_access(c, ea, size_f, opc, rt, dev_base, dev, rd, wr);
        if (ok) c->pc += 4;
        return ok;
    }

    /* Load/store pair, all four modes:
     *   opc 101 V 0 mode L imm7 Rt2 Rn Rt */
    if ((insn & 0x3A000000u) == 0x28000000u) {
        unsigned opc = insn >> 30, mode = (insn >> 23) & 3, load = (insn >> 22) & 1;
        unsigned rt2 = (insn >> 10) & 31;
        int64_t imm7 = mmio_a64_sext((insn >> 15) & 0x7F, 7);
        uint64_t base = mmio_a64_base(c, rn);
        int size, sext = 0;
        uintptr_t ea;
        uint64_t a[2] = {0, 0}, b[2] = {0, 0};

        if (v) {
            if (opc == 3) return 0;
            size = 4 << opc;                        /* S, D, Q */
        } else {
            if (opc == 3) return 0;
            if (opc == 1) {
                if (!load) return 0;                /* STGP: tags, not ours */
                sext = 1;                           /* LDPSW */
                size = 4;
            } else {
                size = opc ? 8 : 4;
            }
        }
        ea = (uintptr_t)(mode == 1 ? base : base + (uint64_t)(imm7 * size));
        if (load) {
            mmio_a64_read(ea, dev_base, dev, rd, size, a);
            mmio_a64_read(ea + (uintptr_t)size, dev_base, dev, rd, size, b);
            if (v) {
                mmio_a64_vset(c, rt, a);
                mmio_a64_vset(c, rt2, b);
            } else {
                mmio_a64_set_loaded(c, rt, a[0], size, sext, size == 8 || sext);
                mmio_a64_set_loaded(c, rt2, b[0], size, sext, size == 8 || sext);
            }
        } else {
            if (v) {
                mmio_a64_vget(c, rt, size, a);
                mmio_a64_vget(c, rt2, size, b);
            } else {
                a[0] = mmio_a64_xr(c, rt);
                b[0] = mmio_a64_xr(c, rt2);
            }
            mmio_a64_write(ea, dev_base, dev, wr, size, a);
            mmio_a64_write(ea + (uintptr_t)size, dev_base, dev, wr, size, b);
        }
        if (mode == 1 || mode == 3)
            mmio_a64_base_w(c, rn, base + (uint64_t)(imm7 * size));
        c->pc += 4;
        return 1;
    }

    /* LSE atomics (LDADD, LDCLR, LDEOR, LDSET, LD{S,U}{MAX,MIN}, SWP) and
     * LDAPR: size 111 0 00 A R 1 Rs o3 opc 00 Rn Rt. Through `at` when the
     * caller has one; otherwise a read and a write, atomic against the guest
     * only as far as one trapped access at a time is. */
    if ((insn & 0x3F200C00u) == 0x38200000u) {
        unsigned rs = (insn >> 16) & 31, o3 = (insn >> 15) & 1, opc = (insn >> 12) & 7;
        int size = 1 << size_f, bits = size * 8;
        uintptr_t ea = (uintptr_t)mmio_a64_base(c, rn);
        uint64_t mask = bits == 64 ? ~0ULL : ((1ULL << bits) - 1);
        uint64_t old[2] = {0, 0}, nw[2] = {0, 0}, s = mmio_a64_xr(c, rs) & mask;

        if (o3 && opc == 4) {                       /* LDAPR: a plain load */
            mmio_a64_read(ea, dev_base, dev, rd, size, old);
            mmio_a64_set_loaded(c, rt, old[0], size, 0, size == 8);
            c->pc += 4;
            return 1;
        }
        if (o3 && opc != 0)
            return 0;
        if (at) {
            old[0] = at(dev, (uint32_t)(ea - dev_base), size,
                        o3 ? MMIO_AT_SWP : (int)opc, s, 0, NULL) & mask;
            mmio_a64_set_loaded(c, rt, old[0], size, 0, size == 8);
            c->pc += 4;
            return 1;
        }
        mmio_a64_read(ea, dev_base, dev, rd, size, old);
        if (o3) {
            nw[0] = s;                              /* SWP */
        } else {
            uint64_t m = old[0] & mask;
            switch (opc) {
            case 0: nw[0] = m + s; break;
            case 1: nw[0] = m & ~s; break;
            case 2: nw[0] = m ^ s; break;
            case 3: nw[0] = m | s; break;
            case 4: nw[0] = mmio_a64_sext(m, bits) > mmio_a64_sext(s, bits) ? m : s; break;
            case 5: nw[0] = mmio_a64_sext(m, bits) < mmio_a64_sext(s, bits) ? m : s; break;
            case 6: nw[0] = m > s ? m : s; break;
            case 7: nw[0] = m < s ? m : s; break;
            }
        }
        mmio_a64_write(ea, dev_base, dev, wr, size, nw);
        mmio_a64_set_loaded(c, rt, old[0], size, 0, size == 8);
        c->pc += 4;
        return 1;
    }

    /* Exclusives, load-acquire/store-release and CAS:
     *   size 001000 o2 L o1 Rs o0 Rt2 Rn Rt */
    if ((insn & 0x3F000000u) == 0x08000000u) {
        unsigned o2 = (insn >> 23) & 1, load = (insn >> 22) & 1, o1 = (insn >> 21) & 1;
        unsigned rs = (insn >> 16) & 31;
        int size = 1 << size_f;
        uintptr_t ea = (uintptr_t)mmio_a64_base(c, rn);
        uint64_t d[2] = {0, 0};

        if (!o1) {
            /* LDXR/LDAXR/LDAR/LDLAR, STXR/STLXR/STLR/STLLR. An emulated
             * exclusive store always succeeds: its load was emulated too,
             * or ran against memory nothing else is writing. */
            uint32_t off = (uint32_t)(ea - dev_base);
            if (load) {
                if (at && !o2)
                    d[0] = at(dev, off, size, MMIO_AT_LDX, 0, 0, NULL);
                else
                    mmio_a64_read(ea, dev_base, dev, rd, size, d);
                mmio_a64_set_loaded(c, rt, d[0], size, 0, size == 8);
            } else if (at && !o2) {
                int status = 0;
                at(dev, off, size, MMIO_AT_STX, mmio_a64_xr(c, rt), 0, &status);
                mmio_a64_xw(c, rs, (uint64_t)status);
            } else {
                d[0] = mmio_a64_xr(c, rt);
                mmio_a64_write(ea, dev_base, dev, wr, size, d);
                if (!o2)
                    mmio_a64_xw(c, rs, 0);          /* exclusive status: success */
            }
            c->pc += 4;
            return 1;
        }
        if (o2 && ((insn >> 10) & 31) == 31) {     /* CAS, CASA, CASL, CASAL */
            uint64_t mask = size == 8 ? ~0ULL : ((1ULL << (size * 8)) - 1);
            uint64_t want = mmio_a64_xr(c, rs) & mask;
            if (at) {
                d[0] = at(dev, (uint32_t)(ea - dev_base), size, MMIO_AT_CAS,
                          mmio_a64_xr(c, rt) & mask, want, NULL) & mask;
                mmio_a64_set_loaded(c, rs, d[0], size, 0, size == 8);
                c->pc += 4;
                return 1;
            }
            mmio_a64_read(ea, dev_base, dev, rd, size, d);
            if ((d[0] & mask) == want) {
                uint64_t n[2] = { mmio_a64_xr(c, rt), 0 };
                mmio_a64_write(ea, dev_base, dev, wr, size, n);
            }
            mmio_a64_set_loaded(c, rs, d[0], size, 0, size == 8);
            c->pc += 4;
            return 1;
        }
        return 0;                                   /* pairs: LDXP/STXP/CASP */
    }

    /* DC ZVA, Xt: zero one cache-line-sized block (what memset uses for
     * large runs). The block size is DCZID_EL0's, read by the caller's
     * platform; 64 bytes on every Apple core. */
    if ((insn & 0xFFFFFFE0u) == 0xD50B7420u) {
        uint64_t block = 64, zero[2] = {0, 0};
#if defined(__aarch64__)
        uint64_t dczid;
        __asm__ volatile("mrs %0, dczid_el0" : "=r"(dczid));
        block = 4ULL << (dczid & 0xF);
#endif
        uintptr_t ea = (uintptr_t)(mmio_a64_xr(c, rt) & ~(block - 1));
        for (uint64_t i = 0; i < block; i += 8)
            mmio_a64_write(ea + i, dev_base, dev, wr, 8, zero);
        c->pc += 4;
        return 1;
    }

    return 0;
}

static inline int mmio_a64_emulate(mmio_a64_ctx *c, uint32_t insn, uintptr_t dev_base,
                                   void *dev, mmio_read_fn rd, mmio_write_fn wr)
{
    return mmio_a64_emulate_ex(c, insn, dev_base, dev, rd, wr, NULL);
}

/* ---- ucontext glue ------------------------------------------------------ */

#if defined(__aarch64__) && defined(__APPLE__)
#include <sys/ucontext.h>
static inline void mmio_a64_from_ucontext(mmio_a64_ctx *c, const ucontext_t *uc)
{
    const struct __darwin_mcontext64 *m = uc->uc_mcontext;
    for (int i = 0; i < 29; i++) c->x[i] = m->__ss.__x[i];
    c->x[29] = m->__ss.__fp;
    c->x[30] = m->__ss.__lr;
    c->sp = m->__ss.__sp;
    c->pc = m->__ss.__pc;
    memcpy(c->v, m->__ns.__v, sizeof(c->v));
}
static inline void mmio_a64_to_ucontext(const mmio_a64_ctx *c, ucontext_t *uc)
{
    struct __darwin_mcontext64 *m = uc->uc_mcontext;
    for (int i = 0; i < 29; i++) m->__ss.__x[i] = c->x[i];
    m->__ss.__fp = c->x[29];
    m->__ss.__lr = c->x[30];
    m->__ss.__sp = c->sp;
    m->__ss.__pc = c->pc;
    memcpy(m->__ns.__v, c->v, sizeof(c->v));
}
/* ESR_EL1 as the kernel reported it, for the write bit of a data abort. */
static inline uint64_t mmio_a64_esr(const ucontext_t *uc) { return uc->uc_mcontext->__es.__esr; }
static inline uint64_t mmio_a64_far(const ucontext_t *uc) { return uc->uc_mcontext->__es.__far; }
#define MMIO_A64_HAVE_UCONTEXT 1

#elif defined(__aarch64__) && defined(__linux__)
#include <ucontext.h>
#include <asm/sigcontext.h>
/* Linux keeps the FP/SIMD registers in a record inside __reserved. */
static inline struct fpsimd_context *mmio_a64_fpsimd(const ucontext_t *uc)
{
    struct _aarch64_ctx *h = (struct _aarch64_ctx *)uc->uc_mcontext.__reserved;
    while (h->magic) {
        if (h->magic == FPSIMD_MAGIC)
            return (struct fpsimd_context *)h;
        h = (struct _aarch64_ctx *)((char *)h + h->size);
    }
    return NULL;
}
static inline void mmio_a64_from_ucontext(mmio_a64_ctx *c, const ucontext_t *uc)
{
    const struct fpsimd_context *f = mmio_a64_fpsimd(uc);
    for (int i = 0; i < 31; i++) c->x[i] = uc->uc_mcontext.regs[i];
    c->sp = uc->uc_mcontext.sp;
    c->pc = uc->uc_mcontext.pc;
    if (f) memcpy(c->v, f->vregs, sizeof(c->v)); else memset(c->v, 0, sizeof(c->v));
}
static inline void mmio_a64_to_ucontext(const mmio_a64_ctx *c, ucontext_t *uc)
{
    struct fpsimd_context *f = mmio_a64_fpsimd(uc);
    for (int i = 0; i < 31; i++) uc->uc_mcontext.regs[i] = c->x[i];
    uc->uc_mcontext.sp = c->sp;
    uc->uc_mcontext.pc = c->pc;
    if (f) memcpy(f->vregs, c->v, sizeof(c->v));
}
static inline uint64_t mmio_a64_esr(const ucontext_t *uc)
{
    struct _aarch64_ctx *h = (struct _aarch64_ctx *)uc->uc_mcontext.__reserved;
    while (h->magic) {
        if (h->magic == ESR_MAGIC)
            return ((struct esr_context *)h)->esr;
        h = (struct _aarch64_ctx *)((char *)h + h->size);
    }
    return 0;
}
static inline uint64_t mmio_a64_far(const ucontext_t *uc) { return uc->uc_mcontext.fault_address; }
#define MMIO_A64_HAVE_UCONTEXT 1
#endif

/* From a data-abort ESR: 1 write, 0 read, -1 not a data abort (decode the
 * instruction instead). EC 0x24/0x25 are data aborts from a lower/the same
 * exception level; WnR is bit 6. A cache maintenance op (DC ZVA) reports
 * WnR = 1 as well, which is the right answer for it. */
static inline int mmio_a64_esr_is_write(uint64_t esr)
{
    unsigned ec = (unsigned)(esr >> 26) & 0x3F;
    if (ec != 0x24 && ec != 0x25)
        return -1;
    return (int)((esr >> 6) & 1);
}

#endif /* MMIO_DECODE_A64_H */
