/*
 * mmio_decode.h -- one x86-64 instruction decoder for trapped MMIO.
 *
 * A device whose registers need semantics cannot be plain memory: the reads
 * have to be answered and the writes have to be seen. The way that works here
 * is to leave the page PAGE_NOACCESS, catch the access in a vectored handler,
 * decode the faulting instruction, service it against the device model, and
 * step over it.
 *
 * That decoder existed twice before this file -- once in nv2a_mmio_hook.c and
 * once in apu_mmio_hook.c -- as the same opcode table written out against two
 * different pairs of accessors. This is the same logic with the accessors
 * passed in, so a third device does not need a third copy. The two originals
 * still carry their own and can move onto this whenever they are next touched.
 *
 * Header-only and static inline: one function, one caller per device, and a
 * library for it would be more build wiring than code.
 *
 * The instructions covered are what the XDK device code actually emits against
 * registers -- moves both ways, the immediate forms, the zero-extending loads,
 * and the read-modify-write and flag-setting forms a poll loop is built from.
 * Anything outside that set returns 0 rather than guessing: stepping over an
 * instruction that was not understood corrupts the guest silently, which is
 * far worse than a fault naming the opcode.
 */
#ifndef MMIO_DECODE_H
#define MMIO_DECODE_H

#include <stdint.h>
#include <stddef.h>

/* The decoder is chosen by the CPU, not the OS: it runs wherever the host
 * is x86-64. On Windows the register file is the VEH's CONTEXT; on a POSIX
 * x86-64 host it is mmio_x86_ctx, the same field names filled from the
 * signal's ucontext (mmio_x86_from_ucontext). An arm64 host uses
 * mmio_decode_a64.h instead. */
#if defined(_WIN32)
#include <windows.h>
typedef CONTEXT mmio_x86_ctx;
#define MMIO_X86_DECODER 1
#elif defined(__x86_64__)
typedef struct mmio_x86_ctx {
    uint64_t Rax, Rcx, Rdx, Rbx, Rsp, Rbp, Rsi, Rdi;
    uint64_t R8, R9, R10, R11, R12, R13, R14, R15;
    uint64_t Rip;
    uint32_t EFlags;
} mmio_x86_ctx;
#define MMIO_X86_DECODER 1
#endif

#if defined(MMIO_X86_DECODER)

/* Service one register access. dev is passed straight back to the callbacks.
 * mmio_decode_a64.h declares the same two types. */
#ifndef MMIO_ACCESS_FN_DEFINED
#define MMIO_ACCESS_FN_DEFINED
typedef uint64_t (*mmio_read_fn)(void *dev, uint32_t off, int size);
typedef void     (*mmio_write_fn)(void *dev, uint32_t off, uint64_t val, int size);
#endif

static inline uint64_t *mmio_ctx_reg(mmio_x86_ctx *c, int reg)
{
    switch (reg & 0xF) {
    case 0:  return (uint64_t *)&c->Rax;   case 1:  return (uint64_t *)&c->Rcx;
    case 2:  return (uint64_t *)&c->Rdx;   case 3:  return (uint64_t *)&c->Rbx;
    case 4:  return (uint64_t *)&c->Rsp;   case 5:  return (uint64_t *)&c->Rbp;
    case 6:  return (uint64_t *)&c->Rsi;   case 7:  return (uint64_t *)&c->Rdi;
    case 8:  return (uint64_t *)&c->R8;    case 9:  return (uint64_t *)&c->R9;
    case 10: return (uint64_t *)&c->R10;   case 11: return (uint64_t *)&c->R11;
    case 12: return (uint64_t *)&c->R12;   case 13: return (uint64_t *)&c->R13;
    case 14: return (uint64_t *)&c->R14;   case 15: return (uint64_t *)&c->R15;
    default: return NULL;
    }
}

static inline int mmio_modrm_len(const uint8_t *ip, int rex_b)
{
    uint8_t modrm = *ip;
    int mod = (modrm >> 6) & 3;
    int rm  = (modrm & 7) | (rex_b ? 8 : 0);
    int len = 1;

    if (mod == 3) return 1;
    if ((rm & 7) == 4) len += 1;                    /* SIB    */
    if (mod == 0 && (rm & 7) == 5) len += 4;        /* disp32 */
    else if (mod == 1) len += 1;
    else if (mod == 2) len += 4;
    return len;
}

/* Flags after a compare-shaped operation, so a poll loop branches correctly.
 * ZF, SF and CF only: those are what jz/jnz, js and jb/jae read, and inventing
 * an overflow flag nothing here sets is worse than leaving it alone. */
static inline void mmio_set_flags(mmio_x86_ctx *ctx, uint64_t result, int size,
                                  int carry)
{
    ctx->EFlags &= ~(0x0001u | 0x0040u | 0x0080u | 0x0800u);
    if (size < 8)
        result &= (1ULL << (size * 8)) - 1;
    if (result == 0)
        ctx->EFlags |= 0x0040u;                                   /* ZF */
    if (result & (1ULL << (size * 8 - 1)))
        ctx->EFlags |= 0x0080u;                                   /* SF */
    if (carry)
        ctx->EFlags |= 0x0001u;                                   /* CF */
}

/* 1 if the instruction at ctx->Rip was serviced and Rip advanced past it. */
static inline int mmio_emulate(mmio_x86_ctx *ctx, uint32_t off, void *dev,
                               mmio_read_fn rd, mmio_write_fn wr)
{
    const uint8_t *ip = (const uint8_t *)ctx->Rip;
    int prefix = 0, has66 = 0, rex = 0, has_rex = 0;
    const uint8_t *op;
    int size, rex_w, rex_r, rex_b, mlen, reg;

    if (!rd || !wr)
        return 0;

    for (;;) {
        uint8_t b = ip[prefix];
        if (b == 0x66)                   { has66 = 1; prefix++; }
        else if (b == 0xF2 || b == 0xF3) { prefix++; }
        else if (b >= 0x40 && b <= 0x4F) { rex = b; has_rex = 1; prefix++; }
        else break;
    }
    rex_w = has_rex && (rex & 0x08);
    rex_r = has_rex && (rex & 0x04);
    rex_b = has_rex && (rex & 0x01);

    op   = ip + prefix;
    size = has66 ? 2 : (rex_w ? 8 : 4);

    switch (op[0]) {
    case 0x88: case 0x89:                            /* MOV r/m, r   (write) */
        if (op[0] == 0x88) size = 1;
        mlen = mmio_modrm_len(op + 1, rex_b);
        reg  = ((op[1] >> 3) & 7) | (rex_r ? 8 : 0);
        wr(dev, off, *mmio_ctx_reg(ctx, reg), size);
        ctx->Rip += prefix + 1 + mlen;
        return 1;

    case 0xC7:                                       /* MOV r/m, imm32       */
        mlen = mmio_modrm_len(op + 1, rex_b);
        wr(dev, off, *(const uint32_t *)(op + 1 + mlen), size);
        ctx->Rip += prefix + 1 + mlen + 4;
        return 1;

    case 0xC6:                                       /* MOV r/m8, imm8       */
        mlen = mmio_modrm_len(op + 1, rex_b);
        wr(dev, off, op[1 + mlen], 1);
        ctx->Rip += prefix + 1 + mlen + 1;
        return 1;

    case 0x8A: case 0x8B: {                          /* MOV r, r/m   (read)  */
        uint64_t v, *dst;
        if (op[0] == 0x8A) size = 1;
        mlen = mmio_modrm_len(op + 1, rex_b);
        reg  = ((op[1] >> 3) & 7) | (rex_r ? 8 : 0);
        v    = rd(dev, off, size);
        dst  = mmio_ctx_reg(ctx, reg);
        if (size == 1)      *dst = (*dst & ~0xFFULL)   | (v & 0xFF);
        else if (size == 2) *dst = (*dst & ~0xFFFFULL) | (v & 0xFFFF);
        else if (size == 4) *dst = v & 0xFFFFFFFFULL;  /* 32-bit clears high */
        else                *dst = v;
        ctx->Rip += prefix + 1 + mlen;
        return 1;
    }

    case 0x84: case 0x85: {                          /* TEST r/m, r          */
        uint64_t m, r;
        if (op[0] == 0x84) size = 1;
        mlen = mmio_modrm_len(op + 1, rex_b);
        reg  = ((op[1] >> 3) & 7) | (rex_r ? 8 : 0);
        m    = rd(dev, off, size);
        r    = *mmio_ctx_reg(ctx, reg);
        mmio_set_flags(ctx, m & r, size, 0);
        ctx->Rip += prefix + 1 + mlen;
        return 1;
    }

    case 0x38: case 0x39: {                          /* CMP r/m, r           */
        uint64_t m, r, mask;
        if (op[0] == 0x38) size = 1;
        mlen = mmio_modrm_len(op + 1, rex_b);
        reg  = ((op[1] >> 3) & 7) | (rex_r ? 8 : 0);
        m    = rd(dev, off, size);
        r    = *mmio_ctx_reg(ctx, reg);
        mask = (size < 8) ? ((1ULL << (size * 8)) - 1) : ~0ULL;
        m &= mask; r &= mask;
        mmio_set_flags(ctx, m - r, size, m < r);
        ctx->Rip += prefix + 1 + mlen;
        return 1;
    }

    case 0x08: case 0x09:                            /* OR  r/m, r           */
        if (op[0] == 0x08) size = 1;
        mlen = mmio_modrm_len(op + 1, rex_b);
        reg  = ((op[1] >> 3) & 7) | (rex_r ? 8 : 0);
        wr(dev, off, rd(dev, off, size) | *mmio_ctx_reg(ctx, reg), size);
        ctx->Rip += prefix + 1 + mlen;
        return 1;

    case 0x20: case 0x21:                            /* AND r/m, r           */
        if (op[0] == 0x20) size = 1;
        mlen = mmio_modrm_len(op + 1, rex_b);
        reg  = ((op[1] >> 3) & 7) | (rex_r ? 8 : 0);
        wr(dev, off, rd(dev, off, size) & *mmio_ctx_reg(ctx, reg), size);
        ctx->Rip += prefix + 1 + mlen;
        return 1;

    case 0x0F:
        if (op[1] == 0xB6 || op[1] == 0xB7) {        /* MOVZX r32, r/m8|16   */
            int s = (op[1] == 0xB6) ? 1 : 2;
            mlen  = mmio_modrm_len(op + 2, rex_b);
            reg   = ((op[2] >> 3) & 7) | (rex_r ? 8 : 0);
            *mmio_ctx_reg(ctx, reg) = rd(dev, off, s) & ((1ULL << (s * 8)) - 1);
            ctx->Rip += prefix + 2 + mlen;
            return 1;
        }
        return 0;

    default:
        return 0;
    }
}

/* ---- POSIX x86-64: the register file from a signal's ucontext ---------- */
#if !defined(_WIN32)
/* Linux names the registers (REG_RAX...) only under _GNU_SOURCE, which has
 * to be defined before the first system header -- something only the
 * including file can do. So the glue exists where that file asked for it
 * (fault_emulate.c does); any other includer just gets the decoder. */
#if defined(__linux__) && defined(_GNU_SOURCE)
#include <ucontext.h>
static inline void mmio_x86_from_ucontext(mmio_x86_ctx *c, const ucontext_t *uc)
{
    const greg_t *g = uc->uc_mcontext.gregs;
    c->Rax = g[REG_RAX]; c->Rcx = g[REG_RCX]; c->Rdx = g[REG_RDX]; c->Rbx = g[REG_RBX];
    c->Rsp = g[REG_RSP]; c->Rbp = g[REG_RBP]; c->Rsi = g[REG_RSI]; c->Rdi = g[REG_RDI];
    c->R8  = g[REG_R8];  c->R9  = g[REG_R9];  c->R10 = g[REG_R10]; c->R11 = g[REG_R11];
    c->R12 = g[REG_R12]; c->R13 = g[REG_R13]; c->R14 = g[REG_R14]; c->R15 = g[REG_R15];
    c->Rip = g[REG_RIP]; c->EFlags = (uint32_t)g[REG_EFL];
}
static inline void mmio_x86_to_ucontext(const mmio_x86_ctx *c, ucontext_t *uc)
{
    greg_t *g = uc->uc_mcontext.gregs;
    g[REG_RAX] = c->Rax; g[REG_RCX] = c->Rcx; g[REG_RDX] = c->Rdx; g[REG_RBX] = c->Rbx;
    g[REG_RSP] = c->Rsp; g[REG_RBP] = c->Rbp; g[REG_RSI] = c->Rsi; g[REG_RDI] = c->Rdi;
    g[REG_R8]  = c->R8;  g[REG_R9]  = c->R9;  g[REG_R10] = c->R10; g[REG_R11] = c->R11;
    g[REG_R12] = c->R12; g[REG_R13] = c->R13; g[REG_R14] = c->R14; g[REG_R15] = c->R15;
    g[REG_RIP] = c->Rip; g[REG_EFL] = c->EFlags;
}
#define MMIO_X86_HAVE_UCONTEXT 1
#elif defined(__APPLE__)
#include <sys/ucontext.h>
static inline void mmio_x86_from_ucontext(mmio_x86_ctx *c, const ucontext_t *uc)
{
    const struct __darwin_x86_thread_state64 *s = &uc->uc_mcontext->__ss;
    c->Rax = s->__rax; c->Rcx = s->__rcx; c->Rdx = s->__rdx; c->Rbx = s->__rbx;
    c->Rsp = s->__rsp; c->Rbp = s->__rbp; c->Rsi = s->__rsi; c->Rdi = s->__rdi;
    c->R8  = s->__r8;  c->R9  = s->__r9;  c->R10 = s->__r10; c->R11 = s->__r11;
    c->R12 = s->__r12; c->R13 = s->__r13; c->R14 = s->__r14; c->R15 = s->__r15;
    c->Rip = s->__rip; c->EFlags = (uint32_t)s->__rflags;
}
static inline void mmio_x86_to_ucontext(const mmio_x86_ctx *c, ucontext_t *uc)
{
    struct __darwin_x86_thread_state64 *s = &uc->uc_mcontext->__ss;
    s->__rax = c->Rax; s->__rcx = c->Rcx; s->__rdx = c->Rdx; s->__rbx = c->Rbx;
    s->__rsp = c->Rsp; s->__rbp = c->Rbp; s->__rsi = c->Rsi; s->__rdi = c->Rdi;
    s->__r8  = c->R8;  s->__r9  = c->R9;  s->__r10 = c->R10; s->__r11 = c->R11;
    s->__r12 = c->R12; s->__r13 = c->R13; s->__r14 = c->R14; s->__r15 = c->R15;
    s->__rip = c->Rip; s->__rflags = c->EFlags;
}
#define MMIO_X86_HAVE_UCONTEXT 1
#endif
#endif /* !_WIN32 */

#endif /* MMIO_X86_DECODER */
#endif /* MMIO_DECODE_H */
