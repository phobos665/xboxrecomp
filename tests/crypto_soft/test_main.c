/*
 * crypto_soft - the kernel's DES and modular exponentiation against
 * published answers.
 *
 * Two consoles agree on a System Link session only if both compute exactly
 * what the Xbox kernel computes, so "it round-trips" is not enough: a DES
 * with one S-box entry wrong still decrypts what it encrypted. Every check
 * here is against an answer someone else published or computed.
 *
 *   DES        FIPS 46 worked example (key 133457799BBCDFF1)
 *   DES CBC    FIPS 81 appendix ("Now is the time for all ")
 *   3DES       NIST SP 800-67 example ("The qufck brown fox jump")
 *   ModExp     Python's pow() on 768-bit (XNet's Diffie-Hellman size) and
 *              2048-bit odd moduli, and a 512-bit even one
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "xbox_crypto_soft.h"
#include "modexp_vectors.h"

static int failures;

static void hex(const char *s, uint8_t *out, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        unsigned v;
        sscanf(s + 2 * i, "%2x", &v);
        out[i] = (uint8_t)v;
    }
}

static void check_bytes(const char *name, const uint8_t *got, const char *want_hex, size_t n)
{
    uint8_t want[64];
    size_t i;
    hex(want_hex, want, n);
    if (memcmp(got, want, n) != 0) {
        printf("FAIL: %s\n  got  ", name);
        for (i = 0; i < n; i++) printf("%02X", got[i]);
        printf("\n  want %s\n", want_hex);
        failures++;
    }
}

int main(void)
{
    uint8_t key[24], table[XC_DES3_TABLE_BYTES], in[24], out[24], back[24], iv[8];
    unsigned k;

    /* DES, FIPS 46 worked example. */
    hex("133457799BBCDFF1", key, 8);
    hex("0123456789ABCDEF", in, 8);
    xc_des_key_table(0, table, key);
    xc_des_block(0, out, in, table, 1);
    check_bytes("DES encrypt", out, "85E813540F0AB405", 8);
    xc_des_block(0, back, out, table, 0);
    check_bytes("DES decrypt", back, "0123456789ABCDEF", 8);

    /* DES CBC, FIPS 81 appendix C. */
    hex("0123456789ABCDEF", key, 8);
    hex("1234567890ABCDEF", iv, 8);
    memcpy(in, "Now is the time for all ", 24);
    xc_des_key_table(0, table, key);
    xc_des_cbc(0, 24, out, in, table, 1, iv);
    check_bytes("DES CBC encrypt", out,
                "E5C7CDDE872BF27C43E934008C389C0F683788499A7C05F6", 24);
    check_bytes("DES CBC feedback carried", iv, "683788499A7C05F6", 8);
    hex("1234567890ABCDEF", iv, 8);
    xc_des_cbc(0, 24, back, out, table, 0, iv);
    check_bytes("DES CBC decrypt", back,
                "4E6F77206973207468652074696D6520666F7220616C6C20", 24);

    /* The same chain in two calls, as XNet makes them: the feedback vector
     * has to carry the chain across. */
    hex("1234567890ABCDEF", iv, 8);
    xc_des_cbc(0, 8, out, in, table, 1, iv);
    xc_des_cbc(0, 16, out + 8, in + 8, table, 1, iv);
    check_bytes("DES CBC in two calls", out,
                "E5C7CDDE872BF27C43E934008C389C0F683788499A7C05F6", 24);

    /* Triple DES, SP 800-67. */
    hex("0123456789ABCDEF23456789ABCDEF01456789ABCDEF0123", key, 24);
    memcpy(in, "The qufck brown fox jump", 24);
    xc_des_key_table(1, table, key);
    for (k = 0; k < 3; k++)
        xc_des_block(1, out + 8 * k, in + 8 * k, table, 1);
    check_bytes("3DES encrypt", out,
                "A826FD8CE53B855FCCE21C8112256FE668D5C05DD9B6B900", 24);
    for (k = 0; k < 3; k++)
        xc_des_block(1, back + 8 * k, out + 8 * k, table, 0);
    check_bytes("3DES decrypt", back,
                "54686520717566636B2062726F776E20666F78206A756D70", 24);

    /* Parity: the low bit makes each byte's count of ones odd. 0x11 has two,
     * so it loses its low bit. */
    {
        uint8_t p[4] = { 0x00, 0xFE, 0x10, 0x11 };
        xc_des_parity(p, 4);
        check_bytes("DES key parity", p, "01FE1010", 4);
    }

    /* Modular exponentiation. */
    for (k = 0; k < sizeof MX / sizeof MX[0]; k++) {
        uint32_t r[XC_BN_MAX_WORDS];
        char name[64];
        memset(r, 0xAA, sizeof r);
        snprintf(name, sizeof name, "ModExp %u bits (%s modulus)",
                 MX[k].n * 32u, (MX[k].m[0] & 1u) ? "odd" : "even");
        if (!xc_modexp(r, MX[k].b, MX[k].e, MX[k].m, MX[k].n) ||
            memcmp(r, MX[k].r, MX[k].n * 4u) != 0) {
            printf("FAIL: %s\n", name);
            failures++;
        }
    }
    {
        /* 4^13 mod 497 = 445, the textbook one, in place. */
        uint32_t b[1] = { 4 }, e[1] = { 13 }, m[1] = { 497 };
        if (!xc_modexp(b, b, e, m, 1) || b[0] != 445) {
            printf("FAIL: ModExp 4^13 mod 497 in place (got %u)\n", b[0]);
            failures++;
        }
    }

    if (failures)
        printf("%d failure(s)\n", failures);
    else
        printf("crypto_soft: all checks pass\n");
    return failures ? 1 : 0;
}
