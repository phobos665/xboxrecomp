/**
 * Software big-number and DES primitives behind the kernel's Xc* exports.
 *
 * Plain C with no kernel dependencies, so they can be checked against known
 * answers on their own (tests/crypto_soft). See xbox_crypto_soft.c.
 */
#ifndef XBOX_CRYPTO_SOFT_H
#define XBOX_CRYPTO_SOFT_H

#include <stdint.h>

/* The largest operand, in 32-bit words: 4096 bits. */
#define XC_BN_MAX_WORDS 128

/* result = base ^ exponent mod modulus. Every number is n little-endian
 * 32-bit words, as XcModExp passes them. Returns 1, or 0 for a zero modulus
 * or n out of range (result untouched). result may alias base. */
int xc_modexp(uint32_t *result, const uint32_t *base, const uint32_t *exponent,
              const uint32_t *modulus, uint32_t n);

/* A DES key schedule is 16 48-bit subkeys, one per 8 bytes: 128 bytes, the
 * size of the Xbox's DES key table. Triple DES is three of them, 384. */
#define XC_DES_TABLE_BYTES  128u
#define XC_DES3_TABLE_BYTES 384u

/* Set odd parity in the low bit of each key byte. */
void xc_des_parity(uint8_t *key, uint32_t len);

/* Fill a key table from an 8-byte (DES) or 24-byte (triple DES) key. */
void xc_des_key_table(int triple, uint8_t *table, const uint8_t *key);

/* One 8-byte block; encrypt non-zero to encrypt, 0 to decrypt. Triple DES
 * is encrypt-decrypt-encrypt with the three keys in order. */
void xc_des_block(int triple, uint8_t out[8], const uint8_t in[8],
                  const uint8_t *table, int encrypt);

/* len bytes (whole blocks; a trailing partial block is left alone) in
 * cipher block chaining, with the 8-byte feedback vector updated in place
 * so the next call continues the chain. out may alias in. */
void xc_des_cbc(int triple, uint32_t len, uint8_t *out, const uint8_t *in,
                const uint8_t *table, int encrypt, uint8_t feedback[8]);

#endif /* XBOX_CRYPTO_SOFT_H */
