/**
 * Software big-number and DES primitives behind the kernel's Xc* exports.
 *
 * System Link needs both. XNet's key exchange is Diffie-Hellman through
 * XcModExp, and its traffic is triple DES in CBC through XcKeyTable and
 * XcBlockCryptCBC; while those were stubs every console computed a
 * different shared secret, and TimeSplitters 2's host rejected every packet
 * a joiner sent after the first exchange. Two consoles only agree if both
 * compute exactly what the Xbox kernel does, so these are the standard
 * algorithms with nothing of our own in them:
 *
 *   - modular exponentiation by left-to-right square-and-multiply with
 *     Montgomery multiplication (odd moduli, which every DH prime and RSA
 *     modulus is), and plain shift-and-subtract for an even one;
 *   - DES as FIPS 46-3 specifies it, and triple DES as
 *     encrypt-decrypt-encrypt with three keys in order;
 *   - CBC as Microsoft's crypto library does it, the feedback vector
 *     carried in place from call to call.
 *
 * tests/crypto_soft checks them against published answers.
 */

#include <string.h>

#include "xbox_crypto_soft.h"

/* ---- big numbers ------------------------------------------------------ */

static int bn_cmp(const uint32_t *a, const uint32_t *b, uint32_t n)
{
    while (n--) {
        if (a[n] != b[n])
            return a[n] < b[n] ? -1 : 1;
    }
    return 0;
}

/* a -= b, returning the borrow. */
static uint32_t bn_sub(uint32_t *a, const uint32_t *b, uint32_t n)
{
    uint64_t borrow = 0;
    uint32_t i;
    for (i = 0; i < n; i++) {
        uint64_t d = (uint64_t)a[i] - b[i] - borrow;
        a[i] = (uint32_t)d;
        borrow = (d >> 63) & 1u;
    }
    return (uint32_t)borrow;
}

/* r = (2r + bit) mod m, for r < m. */
static void bn_double_add_mod(uint32_t *r, uint32_t bit, const uint32_t *m,
                              uint32_t n)
{
    uint32_t carry = bit, i;
    for (i = 0; i < n; i++) {
        uint32_t top = r[i] >> 31;
        r[i] = (r[i] << 1) | carry;
        carry = top;
    }
    if (carry || bn_cmp(r, m, n) >= 0)
        bn_sub(r, m, n);
}

static int bn_bit(const uint32_t *a, uint32_t i)
{
    return (a[i / 32u] >> (i % 32u)) & 1u;
}

/* r = a mod m, bit by bit: any a, any non-zero m. */
static void bn_mod(uint32_t *r, const uint32_t *a, const uint32_t *m, uint32_t n)
{
    uint32_t i = n * 32u;
    memset(r, 0, n * 4u);
    while (i--)
        bn_double_add_mod(r, (uint32_t)bn_bit(a, i), m, n);
}

/* r = a * b mod m by shift-and-add, for the even-modulus case. a, b < m. */
static void bn_mulmod_slow(uint32_t *r, const uint32_t *a, const uint32_t *b,
                           const uint32_t *m, uint32_t n)
{
    uint32_t acc[XC_BN_MAX_WORDS], i = n * 32u;
    memset(acc, 0, n * 4u);
    while (i--) {
        bn_double_add_mod(acc, 0, m, n);
        if (bn_bit(a, i)) {
            /* acc = acc + b mod m */
            uint64_t c = 0;
            uint32_t j;
            for (j = 0; j < n; j++) {
                uint64_t s = (uint64_t)acc[j] + b[j] + c;
                acc[j] = (uint32_t)s;
                c = s >> 32;
            }
            if (c || bn_cmp(acc, m, n) >= 0)
                bn_sub(acc, m, n);
        }
    }
    memcpy(r, acc, n * 4u);
}

/* Montgomery product: r = a * b / 2^(32n) mod m, for odd m and a, b < m.
 * m0inv is -1/m[0] mod 2^32. The coarsely integrated operand scanning form:
 * one word of b at a time, reducing as it goes. */
static void mont_mul(uint32_t *r, const uint32_t *a, const uint32_t *b,
                     const uint32_t *m, uint32_t m0inv, uint32_t n)
{
    uint32_t t[XC_BN_MAX_WORDS + 2];
    uint32_t i, j;

    memset(t, 0, (n + 2u) * 4u);
    for (i = 0; i < n; i++) {
        uint64_t c = 0, s;
        uint32_t u;
        for (j = 0; j < n; j++) {
            s = (uint64_t)t[j] + (uint64_t)a[j] * b[i] + c;
            t[j] = (uint32_t)s;
            c = s >> 32;
        }
        s = (uint64_t)t[n] + c;
        t[n] = (uint32_t)s;
        t[n + 1] = (uint32_t)(s >> 32);

        u = t[0] * m0inv;
        s = (uint64_t)t[0] + (uint64_t)u * m[0];
        c = s >> 32;
        for (j = 1; j < n; j++) {
            s = (uint64_t)t[j] + (uint64_t)u * m[j] + c;
            t[j - 1] = (uint32_t)s;
            c = s >> 32;
        }
        s = (uint64_t)t[n] + c;
        t[n - 1] = (uint32_t)s;
        t[n] = t[n + 1] + (uint32_t)(s >> 32);
        t[n + 1] = 0;
    }
    if (t[n] || bn_cmp(t, m, n) >= 0)
        bn_sub(t, m, n);
    memcpy(r, t, n * 4u);
}

int xc_modexp(uint32_t *result, const uint32_t *base, const uint32_t *exponent,
              const uint32_t *modulus, uint32_t n)
{
    uint32_t b[XC_BN_MAX_WORDS], x[XC_BN_MAX_WORDS], one[XC_BN_MAX_WORDS];
    uint32_t top, i, zero = 1;

    if (n == 0 || n > XC_BN_MAX_WORDS)
        return 0;
    for (i = 0; i < n; i++)
        if (modulus[i])
            zero = 0;
    if (zero)
        return 0;

    bn_mod(b, base, modulus, n);
    memset(one, 0, n * 4u);
    one[0] = 1;

    /* The exponent's highest set bit; an exponent of zero gives 1 mod m. */
    top = n * 32u;
    while (top && !bn_bit(exponent, top - 1u))
        top--;

    if (modulus[0] & 1u) {
        uint32_t rr[XC_BN_MAX_WORDS], inv = modulus[0], k;

        /* -1/m[0] mod 2^32 by Newton's iteration: each step doubles the
         * correct low bits, and m[0] itself is right to three. */
        for (k = 0; k < 4; k++)
            inv *= 2u - modulus[0] * inv;
        inv = 0u - inv;

        /* R^2 mod m, R = 2^(32n): 1 doubled 64n times. */
        memset(rr, 0, n * 4u);
        rr[0] = 1;
        if (bn_cmp(rr, modulus, n) >= 0)        /* m == 1 */
            memset(rr, 0, n * 4u);
        for (k = 0; k < n * 64u; k++)
            bn_double_add_mod(rr, 0, modulus, n);

        mont_mul(b, b, rr, modulus, inv, n);    /* b * R mod m   */
        mont_mul(x, one, rr, modulus, inv, n);  /* 1 * R mod m   */
        for (i = top; i-- > 0; ) {
            mont_mul(x, x, x, modulus, inv, n);
            if (bn_bit(exponent, i))
                mont_mul(x, x, b, modulus, inv, n);
        }
        mont_mul(x, x, one, modulus, inv, n);   /* out of the domain */
    } else {
        bn_mod(x, one, modulus, n);
        for (i = top; i-- > 0; ) {
            bn_mulmod_slow(x, x, x, modulus, n);
            if (bn_bit(exponent, i))
                bn_mulmod_slow(x, x, b, modulus, n);
        }
    }
    memcpy(result, x, n * 4u);
    return 1;
}

/* ---- DES ---------------------------------------------------------------- */

/* FIPS 46-3. Bit positions count from 1 at the most significant bit. */
static const uint8_t IP[64] = {
    58, 50, 42, 34, 26, 18, 10, 2, 60, 52, 44, 36, 28, 20, 12, 4,
    62, 54, 46, 38, 30, 22, 14, 6, 64, 56, 48, 40, 32, 24, 16, 8,
    57, 49, 41, 33, 25, 17,  9, 1, 59, 51, 43, 35, 27, 19, 11, 3,
    61, 53, 45, 37, 29, 21, 13, 5, 63, 55, 47, 39, 31, 23, 15, 7 };
static const uint8_t FP[64] = {
    40, 8, 48, 16, 56, 24, 64, 32, 39, 7, 47, 15, 55, 23, 63, 31,
    38, 6, 46, 14, 54, 22, 62, 30, 37, 5, 45, 13, 53, 21, 61, 29,
    36, 4, 44, 12, 52, 20, 60, 28, 35, 3, 43, 11, 51, 19, 59, 27,
    34, 2, 42, 10, 50, 18, 58, 26, 33, 1, 41,  9, 49, 17, 57, 25 };
static const uint8_t E[48] = {
    32,  1,  2,  3,  4,  5,  4,  5,  6,  7,  8,  9,
     8,  9, 10, 11, 12, 13, 12, 13, 14, 15, 16, 17,
    16, 17, 18, 19, 20, 21, 20, 21, 22, 23, 24, 25,
    24, 25, 26, 27, 28, 29, 28, 29, 30, 31, 32,  1 };
static const uint8_t P[32] = {
    16,  7, 20, 21, 29, 12, 28, 17,  1, 15, 23, 26,  5, 18, 31, 10,
     2,  8, 24, 14, 32, 27,  3,  9, 19, 13, 30,  6, 22, 11,  4, 25 };
static const uint8_t PC1[56] = {
    57, 49, 41, 33, 25, 17,  9,  1, 58, 50, 42, 34, 26, 18,
    10,  2, 59, 51, 43, 35, 27, 19, 11,  3, 60, 52, 44, 36,
    63, 55, 47, 39, 31, 23, 15,  7, 62, 54, 46, 38, 30, 22,
    14,  6, 61, 53, 45, 37, 29, 21, 13,  5, 28, 20, 12,  4 };
static const uint8_t PC2[48] = {
    14, 17, 11, 24,  1,  5,  3, 28, 15,  6, 21, 10,
    23, 19, 12,  4, 26,  8, 16,  7, 27, 20, 13,  2,
    41, 52, 31, 37, 47, 55, 30, 40, 51, 45, 33, 48,
    44, 49, 39, 56, 34, 53, 46, 42, 50, 36, 29, 32 };
static const uint8_t SHIFTS[16] = { 1, 1, 2, 2, 2, 2, 2, 2, 1, 2, 2, 2, 2, 2, 2, 1 };
static const uint8_t S[8][64] = {
    { 14,  4, 13,  1,  2, 15, 11,  8,  3, 10,  6, 12,  5,  9,  0,  7,
       0, 15,  7,  4, 14,  2, 13,  1, 10,  6, 12, 11,  9,  5,  3,  8,
       4,  1, 14,  8, 13,  6,  2, 11, 15, 12,  9,  7,  3, 10,  5,  0,
      15, 12,  8,  2,  4,  9,  1,  7,  5, 11,  3, 14, 10,  0,  6, 13 },
    { 15,  1,  8, 14,  6, 11,  3,  4,  9,  7,  2, 13, 12,  0,  5, 10,
       3, 13,  4,  7, 15,  2,  8, 14, 12,  0,  1, 10,  6,  9, 11,  5,
       0, 14,  7, 11, 10,  4, 13,  1,  5,  8, 12,  6,  9,  3,  2, 15,
      13,  8, 10,  1,  3, 15,  4,  2, 11,  6,  7, 12,  0,  5, 14,  9 },
    { 10,  0,  9, 14,  6,  3, 15,  5,  1, 13, 12,  7, 11,  4,  2,  8,
      13,  7,  0,  9,  3,  4,  6, 10,  2,  8,  5, 14, 12, 11, 15,  1,
      13,  6,  4,  9,  8, 15,  3,  0, 11,  1,  2, 12,  5, 10, 14,  7,
       1, 10, 13,  0,  6,  9,  8,  7,  4, 15, 14,  3, 11,  5,  2, 12 },
    {  7, 13, 14,  3,  0,  6,  9, 10,  1,  2,  8,  5, 11, 12,  4, 15,
      13,  8, 11,  5,  6, 15,  0,  3,  4,  7,  2, 12,  1, 10, 14,  9,
      10,  6,  9,  0, 12, 11,  7, 13, 15,  1,  3, 14,  5,  2,  8,  4,
       3, 15,  0,  6, 10,  1, 13,  8,  9,  4,  5, 11, 12,  7,  2, 14 },
    {  2, 12,  4,  1,  7, 10, 11,  6,  8,  5,  3, 15, 13,  0, 14,  9,
      14, 11,  2, 12,  4,  7, 13,  1,  5,  0, 15, 10,  3,  9,  8,  6,
       4,  2,  1, 11, 10, 13,  7,  8, 15,  9, 12,  5,  6,  3,  0, 14,
      11,  8, 12,  7,  1, 14,  2, 13,  6, 15,  0,  9, 10,  4,  5,  3 },
    { 12,  1, 10, 15,  9,  2,  6,  8,  0, 13,  3,  4, 14,  7,  5, 11,
      10, 15,  4,  2,  7, 12,  9,  5,  6,  1, 13, 14,  0, 11,  3,  8,
       9, 14, 15,  5,  2,  8, 12,  3,  7,  0,  4, 10,  1, 13, 11,  6,
       4,  3,  2, 12,  9,  5, 15, 10, 11, 14,  1,  7,  6,  0,  8, 13 },
    {  4, 11,  2, 14, 15,  0,  8, 13,  3, 12,  9,  7,  5, 10,  6,  1,
      13,  0, 11,  7,  4,  9,  1, 10, 14,  3,  5, 12,  2, 15,  8,  6,
       1,  4, 11, 13, 12,  3,  7, 14, 10, 15,  6,  8,  0,  5,  9,  2,
       6, 11, 13,  8,  1,  4, 10,  7,  9,  5,  0, 15, 14,  2,  3, 12 },
    { 13,  2,  8,  4,  6, 15, 11,  1, 10,  9,  3, 14,  5,  0, 12,  7,
       1, 15, 13,  8, 10,  3,  7,  4, 12,  5,  6, 11,  0, 14,  9,  2,
       7, 11,  4,  1,  9, 12, 14,  2,  0,  6, 10, 13, 15,  3,  5,  8,
       2,  1, 14,  7,  4, 10,  8, 13, 15, 12,  9,  0,  3,  5,  6, 11 } };

/* Pick bits out of an in_bits-wide value by a table of 1-based positions
 * counted from the most significant bit. */
static uint64_t permute(uint64_t in, int in_bits, const uint8_t *table, int out_bits)
{
    uint64_t out = 0;
    int i;
    for (i = 0; i < out_bits; i++)
        out = (out << 1) | ((in >> (in_bits - table[i])) & 1u);
    return out;
}

static uint64_t load_be64(const uint8_t *p)
{
    uint64_t v = 0;
    int i;
    for (i = 0; i < 8; i++)
        v = (v << 8) | p[i];
    return v;
}

static void store_be64(uint8_t *p, uint64_t v)
{
    int i;
    for (i = 7; i >= 0; i--) {
        p[i] = (uint8_t)v;
        v >>= 8;
    }
}

static void des_schedule(uint64_t sub[16], const uint8_t key[8])
{
    uint64_t cd = permute(load_be64(key), 64, PC1, 56);
    uint32_t c = (uint32_t)(cd >> 28) & 0x0FFFFFFFu;
    uint32_t d = (uint32_t)cd & 0x0FFFFFFFu;
    int r;
    for (r = 0; r < 16; r++) {
        int s = SHIFTS[r];
        c = ((c << s) | (c >> (28 - s))) & 0x0FFFFFFFu;
        d = ((d << s) | (d >> (28 - s))) & 0x0FFFFFFFu;
        sub[r] = permute(((uint64_t)c << 28) | d, 56, PC2, 48);
    }
}

static uint32_t feistel(uint32_t r, uint64_t k)
{
    uint64_t x = permute(r, 32, E, 48) ^ k;
    uint32_t out = 0;
    int i;
    for (i = 0; i < 8; i++) {
        uint32_t six = (uint32_t)(x >> (42 - 6 * i)) & 0x3Fu;
        uint32_t row = ((six & 0x20u) >> 4) | (six & 1u);
        uint32_t col = (six >> 1) & 0xFu;
        out = (out << 4) | S[i][row * 16u + col];
    }
    return (uint32_t)permute(out, 32, P, 32);
}

static uint64_t des_crypt(uint64_t block, const uint64_t sub[16], int encrypt)
{
    uint64_t ip = permute(block, 64, IP, 64);
    uint32_t l = (uint32_t)(ip >> 32), r = (uint32_t)ip;
    int i;
    for (i = 0; i < 16; i++) {
        uint32_t t = r;
        r = l ^ feistel(r, sub[encrypt ? i : 15 - i]);
        l = t;
    }
    return permute(((uint64_t)r << 32) | l, 64, FP, 64);
}

/* The table stores each subkey as 8 little-endian bytes. */
static void table_load(uint64_t sub[16], const uint8_t *table)
{
    memcpy(sub, table, 16 * 8);
}

void xc_des_parity(uint8_t *key, uint32_t len)
{
    uint32_t i;
    for (i = 0; i < len; i++) {
        uint8_t b = key[i] & 0xFEu, ones = 0, t = b;
        while (t) {
            ones += t & 1u;
            t >>= 1;
        }
        key[i] = (uint8_t)(b | ((ones & 1u) ? 0u : 1u));
    }
}

void xc_des_key_table(int triple, uint8_t *table, const uint8_t *key)
{
    uint64_t sub[16];
    int k, keys = triple ? 3 : 1;
    for (k = 0; k < keys; k++) {
        des_schedule(sub, key + 8 * k);
        memcpy(table + XC_DES_TABLE_BYTES * (uint32_t)k, sub, sizeof sub);
    }
}

void xc_des_block(int triple, uint8_t out[8], const uint8_t in[8],
                  const uint8_t *table, int encrypt)
{
    uint64_t sub[16], v = load_be64(in);

    if (!triple) {
        table_load(sub, table);
        v = des_crypt(v, sub, encrypt);
    } else if (encrypt) {
        table_load(sub, table);                            v = des_crypt(v, sub, 1);
        table_load(sub, table + XC_DES_TABLE_BYTES);       v = des_crypt(v, sub, 0);
        table_load(sub, table + 2 * XC_DES_TABLE_BYTES);   v = des_crypt(v, sub, 1);
    } else {
        table_load(sub, table + 2 * XC_DES_TABLE_BYTES);   v = des_crypt(v, sub, 0);
        table_load(sub, table + XC_DES_TABLE_BYTES);       v = des_crypt(v, sub, 1);
        table_load(sub, table);                            v = des_crypt(v, sub, 0);
    }
    store_be64(out, v);
}

void xc_des_cbc(int triple, uint32_t len, uint8_t *out, const uint8_t *in,
                const uint8_t *table, int encrypt, uint8_t feedback[8])
{
    uint32_t off;
    for (off = 0; off + 8u <= len; off += 8u) {
        uint8_t block[8], saved[8];
        int i;
        if (encrypt) {
            for (i = 0; i < 8; i++)
                block[i] = in[off + i] ^ feedback[i];
            xc_des_block(triple, out + off, block, table, 1);
            memcpy(feedback, out + off, 8);
        } else {
            memcpy(saved, in + off, 8);
            xc_des_block(triple, block, saved, table, 0);
            for (i = 0; i < 8; i++)
                out[off + i] = block[i] ^ feedback[i];
            memcpy(feedback, saved, 8);
        }
    }
}
