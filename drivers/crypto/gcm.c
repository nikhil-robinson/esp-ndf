/* AES-GCM on top of aes_crypt (CTR, DMA for long runs); GHASH with Shoup's 8-bit table (4 KB; mbedtls uses 4-bit). See aes.h. */
#include "aes.h"
#include <string.h>

typedef struct { uint32_t m[256][4]; uint8_t y[16]; } ghash_t;     /* m[i] = i * H; 128-bit values as big-endian words */
static uint16_t r8[256];                                        /* reduction of the byte shifted out by z * x^8 (top 16 bits) */

static uint32_t ldw(const uint8_t *b) { return (uint32_t)b[0] << 24 | b[1] << 16 | b[2] << 8 | b[3]; }
static void stw(uint8_t *b, uint32_t v) { b[0] = v >> 24; b[1] = v >> 16; b[2] = v >> 8; b[3] = v; }
static void put64(uint8_t *b, uint64_t v) { for (int i = 7; i >= 0; i--) { b[i] = v; v >>= 8; } }
static void mulx(uint32_t *d, const uint32_t *v)                /* d = v * x in GCM's reflected order */
{
    uint32_t lsb = v[3] & 1;
    d[3] = v[3] >> 1 | v[2] << 31; d[2] = v[2] >> 1 | v[1] << 31; d[1] = v[1] >> 1 | v[0] << 31;
    d[0] = v[0] >> 1 ^ (-lsb & 0xe1000000u);
}

static void gh_init(ghash_t *g, const uint8_t h[16])
{
    if (!r8[1]) for (int b = 1; b < 256; b++) {
        uint32_t v[4] = { 0, 0, 0, (uint32_t)b };
        for (int k = 0; k < 8; k++) mulx(v, v);
        r8[b] = v[0] >> 16;
    }
    memset(g->m[0], 0, 16);
    for (int i = 0; i < 4; i++) g->m[128][i] = ldw(h + 4 * i);
    for (int i = 64; i > 0; i >>= 1) mulx(g->m[i], g->m[2 * i]);
    for (int i = 2; i < 256; i *= 2) {
        uint32_t a0 = g->m[i][0], a1 = g->m[i][1], a2 = g->m[i][2], a3 = g->m[i][3];
        uint32_t (*d)[4] = &g->m[i + 1], (*s)[4] = &g->m[1];
        for (int j = 1; j < i; j++, d++, s++) { (*d)[0] = a0 ^ (*s)[0]; (*d)[1] = a1 ^ (*s)[1]; (*d)[2] = a2 ^ (*s)[2]; (*d)[3] = a3 ^ (*s)[3]; }
    }
    memset(g->y, 0, 16);
}
static void gh_mul(ghash_t *g)                                  /* y = y * H, one byte per step (Shoup 8-bit) */
{
    const uint8_t *x = g->y;
    const uint32_t *t = g->m[x[15]];
    uint32_t z0 = t[0], z1 = t[1], z2 = t[2], z3 = t[3];
    for (int i = 14; i >= 0; i--) {
        uint32_t rem = z3 & 255;
        z3 = z3 >> 8 | z2 << 24; z2 = z2 >> 8 | z1 << 24; z1 = z1 >> 8 | z0 << 24; z0 = z0 >> 8 ^ (uint32_t)r8[rem] << 16;
        t = g->m[x[i]]; z0 ^= t[0]; z1 ^= t[1]; z2 ^= t[2]; z3 ^= t[3];
    }
    stw(g->y, z0); stw(g->y + 4, z1); stw(g->y + 8, z2); stw(g->y + 12, z3);
}
static void gh_update(ghash_t *g, const uint8_t *p, size_t n)   /* zero-padded to 16 */
{
    for (; n; ) { size_t k = n < 16 ? n : 16; for (size_t i = 0; i < k; i++) g->y[i] ^= p[i]; gh_mul(g); p += k; n -= k; }
}

int aes_gcm(const uint8_t *key, int key_len, bool encrypt, const uint8_t *iv, size_t iv_len, const void *aad, size_t aad_len,
            const void *in, void *out, size_t len, uint8_t tag[16])
{
    static ghash_t g;                                           /* ponytail: one caller at a time (4 KB not on the stack) */
    uint8_t h[16] = { 0 }, j0[16], ctr[16], ek[16], lens[16];
    aes_block(key, key_len, true, h, h);
    gh_init(&g, h);
    if (iv_len == 12) { memcpy(j0, iv, 12); j0[12] = j0[13] = j0[14] = 0; j0[15] = 1; }
    else {
        gh_update(&g, iv, iv_len);
        memset(lens, 0, 8); put64(lens + 8, (uint64_t)iv_len * 8); gh_update(&g, lens, 16);
        memcpy(j0, g.y, 16); memset(g.y, 0, 16);
    }
    aes_block(key, key_len, true, j0, ek);
    memcpy(ctr, j0, 16);
    for (int i = 15; i >= 12 && !++ctr[i]; i--) {}              /* inc32 */
    gh_update(&g, aad, aad_len);
    if (encrypt) { aes_crypt(AES_CTR, key, key_len, true, ctr, in, out, len); gh_update(&g, out, len); }
    else gh_update(&g, in, len);
    put64(lens, (uint64_t)aad_len * 8); put64(lens + 8, (uint64_t)len * 8); gh_update(&g, lens, 16);
    for (int i = 0; i < 16; i++) g.y[i] ^= ek[i];
    if (encrypt) { memcpy(tag, g.y, 16); return 0; }
    uint8_t d = 0; for (int i = 0; i < 16; i++) d |= g.y[i] ^ tag[i];
    if (d) { memset(out, 0, len); return -1; }
    aes_crypt(AES_CTR, key, key_len, true, ctr, in, out, len);
    return 0;
}
