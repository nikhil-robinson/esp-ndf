/* Security Manager cryptographic toolbox (Core spec vol 3 part H 2.2): f4, f5, f6, g2, ah. See smp.h for the byte-order convention. */
#include "smp.h"
#include "aes.h"
#include <string.h>

static void rev(uint8_t *o, const uint8_t *i, unsigned n) { for (unsigned k = 0; k < n; k++) o[k] = i[n - 1 - k]; }     /* o and i must not overlap */

static void dbl(uint8_t b[16]) { uint8_t c = b[0] >> 7; for (int i = 0; i < 15; i++) b[i] = (uint8_t)(b[i] << 1 | b[i + 1] >> 7); b[15] = (uint8_t)(b[15] << 1) ^ (c ? 0x87 : 0); }

void smp_cmac(const uint8_t key[16], const uint8_t *m, unsigned n, uint8_t out[16])
{
    uint8_t k[16] = { 0 }, x[16] = { 0 }, blk[16];
    aes_block(key, 16, true, k, k); dbl(k);                                          /* K1 */
    unsigned full = n && n % 16 == 0 ? n / 16 - 1 : n / 16, i;
    for (i = 0; i < full; i++) { for (int t = 0; t < 16; t++) x[t] ^= m[16 * i + t]; aes_block(key, 16, true, x, x); }
    unsigned rest = n - 16 * full;
    memset(blk, 0, 16); memcpy(blk, m + 16 * full, rest);
    if (rest == 16) { for (int t = 0; t < 16; t++) blk[t] ^= k[t]; }
    else { blk[rest] = 0x80; dbl(k); for (int t = 0; t < 16; t++) blk[t] ^= k[t]; }                       /* padded: K2 */
    for (int t = 0; t < 16; t++) x[t] ^= blk[t];
    aes_block(key, 16, true, x, out);
}

void smp_e(const uint8_t key[16], const uint8_t in[16], uint8_t out[16])
{
    uint8_t k[16], p[16], c[16]; rev(k, key, 16); rev(p, in, 16);
    aes_block(k, 16, true, p, c); rev(out, c, 16);
}

void smp_f4(const uint8_t u[32], const uint8_t v[32], const uint8_t x[16], uint8_t z, uint8_t out[16])
{
    uint8_t m[65], k[16], c[16]; rev(m, u, 32); rev(m + 32, v, 32); m[64] = z; rev(k, x, 16);
    smp_cmac(k, m, 65, c); rev(out, c, 16);
}

void smp_f5(const uint8_t w[32], const uint8_t n1[16], const uint8_t n2[16], const uint8_t a1[7], const uint8_t a2[7], uint8_t mackey[16], uint8_t ltk[16])
{
    static const uint8_t salt[16] = { 0x6C, 0x88, 0x83, 0x91, 0xAA, 0xF5, 0xA5, 0x38, 0x60, 0x37, 0x0B, 0xDB, 0x5A, 0x60, 0x83, 0xBE };
    uint8_t wb[32], t[16], m[53], c[16];
    rev(wb, w, 32); smp_cmac(salt, wb, 32, t);
    m[0] = 0; m[1] = 0x62; m[2] = 0x74; m[3] = 0x6C; m[4] = 0x65;                                  /* counter, "btle" */
    rev(m + 5, n1, 16); rev(m + 21, n2, 16);
    m[37] = a1[0]; rev(m + 38, a1 + 1, 6); m[44] = a2[0]; rev(m + 45, a2 + 1, 6);
    m[51] = 0x01; m[52] = 0x00;                                                                    /* length 256 */
    smp_cmac(t, m, 53, c); rev(mackey, c, 16);
    m[0] = 1; smp_cmac(t, m, 53, c); rev(ltk, c, 16);
}

void smp_f6(const uint8_t w[16], const uint8_t n1[16], const uint8_t n2[16], const uint8_t r[16], const uint8_t io[3], const uint8_t a1[7], const uint8_t a2[7], uint8_t out[16])
{
    uint8_t m[65], k[16], c[16];
    rev(k, w, 16); rev(m, n1, 16); rev(m + 16, n2, 16); rev(m + 32, r, 16); rev(m + 48, io, 3);
    m[51] = a1[0]; rev(m + 52, a1 + 1, 6); m[58] = a2[0]; rev(m + 59, a2 + 1, 6);
    smp_cmac(k, m, 65, c); rev(out, c, 16);
}

uint32_t smp_g2(const uint8_t u[32], const uint8_t v[32], const uint8_t x[16], const uint8_t y[16])
{
    uint8_t m[80], k[16], c[16]; rev(m, u, 32); rev(m + 32, v, 32); rev(m + 64, y, 16); rev(k, x, 16);
    smp_cmac(k, m, 80, c);
    return (uint32_t)c[12] << 24 | c[13] << 16 | c[14] << 8 | c[15];
}

void smp_ah(const uint8_t irk[16], const uint8_t r[3], uint8_t hash[3])
{
    uint8_t p[16] = { 0 }, o[16]; p[0] = r[0]; p[1] = r[1]; p[2] = r[2];                           /* padding in the most significant octets */
    smp_e(irk, p, o); hash[0] = o[0]; hash[1] = o[1]; hash[2] = o[2];
}
