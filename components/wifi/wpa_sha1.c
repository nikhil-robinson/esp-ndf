/* SHA-1, HMAC-SHA1, PBKDF2 and the 802.11 PRF in software: pure, no hardware. PBKDF2 does 8192 compressions with fixed padding, so it keeps
 * the pad states and runs the compression directly (the SHA engine pays a call overhead per block). */
#include <string.h>
#include "wpa.h"
#ifdef __XTENSA__
#include "ndf.h"
#include "sha.h"
#define WPA_HW_SHA 1
#endif
#ifndef RAM_FN
#define RAM_FN
#endif

#define ROL(x, n) ((x) << (n) | (x) >> (32 - (n)))
static void zero(void *p, unsigned n) { volatile uint8_t *q = p; while (n--) *q++ = 0; }

/* hot loop of PBKDF2 (16 384 compressions per passphrase): in RAM (no flash cache misses), rolling 16-word schedule, four unrolled 20-round groups */
#define ROUND(f, k, wi) do { uint32_t t = ROL(a, 5) + (f) + e + (k) + (wi); e = d; d = c; c = ROL(bb, 30); bb = a; a = t; } while (0)
#define SCHED(i) (w[(i) & 15] = ROL(w[((i) - 3) & 15] ^ w[((i) - 8) & 15] ^ w[((i) - 14) & 15] ^ w[(i) & 15], 1))
RAM_FN __attribute__((optimize("O3"))) static void sha1_block(uint32_t h[5], const uint8_t *b)
{
    uint32_t w[16], a = h[0], bb = h[1], c = h[2], d = h[3], e = h[4];
    for (int i = 0; i < 16; i++) w[i] = (uint32_t)b[4 * i] << 24 | b[4 * i + 1] << 16 | b[4 * i + 2] << 8 | b[4 * i + 3];
    for (int i = 0; i < 16; i++) ROUND((bb & c) | (~bb & d), 0x5A827999, w[i]);
    for (int i = 16; i < 20; i++) ROUND((bb & c) | (~bb & d), 0x5A827999, SCHED(i));
    for (int i = 20; i < 40; i++) ROUND(bb ^ c ^ d, 0x6ED9EBA1, SCHED(i));
    for (int i = 40; i < 60; i++) ROUND((bb & c) | (bb & d) | (c & d), 0x8F1BBCDC, SCHED(i));
    for (int i = 60; i < 80; i++) ROUND(bb ^ c ^ d, 0xCA62C1D6, SCHED(i));
    h[0] += a; h[1] += bb; h[2] += c; h[3] += d; h[4] += e;
}
static const uint32_t iv[5] = { 0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0 };
static void put(uint8_t o[20], const uint32_t h[5]) { for (int i = 0; i < 20; i++) o[i] = (uint8_t)(h[i / 4] >> (24 - 8 * (i % 4))); }

typedef struct { uint32_t h[5]; uint64_t total; uint8_t buf[64]; unsigned fill; } sha1_t;
static void s_init(sha1_t *c) { memcpy(c->h, iv, 20); c->total = c->fill = 0; }
static void s_update(sha1_t *c, const uint8_t *d, unsigned n)
{
    c->total += n;
    while (n) { unsigned k = 64 - c->fill; if (k > n) k = n; memcpy(c->buf + c->fill, d, k); c->fill += k; d += k; n -= k; if (c->fill == 64) { sha1_block(c->h, c->buf); c->fill = 0; } }
}
static void s_final(sha1_t *c, uint8_t out[20])
{
    uint64_t bits = c->total * 8; uint8_t pad = 0x80;
    s_update(c, &pad, 1);
    pad = 0; while (c->fill != 56) s_update(c, &pad, 1);
    uint8_t l[8]; for (int i = 0; i < 8; i++) l[i] = (uint8_t)(bits >> (56 - 8 * i));
    s_update(c, l, 8); put(out, c->h);
}

void wpa_hmac_sha1(const uint8_t *key, unsigned klen, const uint8_t *data, unsigned n, uint8_t out[20])
{
    uint8_t k[64] = { 0 }, t[64], in[20]; sha1_t c;
    if (klen > 64) { s_init(&c); s_update(&c, key, klen); s_final(&c, k); } else memcpy(k, key, klen);
    for (int i = 0; i < 64; i++) t[i] = k[i] ^ 0x36;
    s_init(&c); s_update(&c, t, 64); s_update(&c, data, n); s_final(&c, in);
    for (int i = 0; i < 64; i++) t[i] = k[i] ^ 0x5c;
    s_init(&c); s_update(&c, t, 64); s_update(&c, in, 20); s_final(&c, out);
    zero(k, sizeof k); zero(t, sizeof t);
}

/* PBKDF2 with the key's inner/outer pad states kept: each iteration = 2 compressions of a 20-byte message + fixed padding */
static void pbkdf2_block(const uint8_t kpad[64], const uint32_t ist[5], const uint32_t ost[5], const uint8_t *salt, unsigned sl, uint32_t idx, uint8_t t[20])
{
    uint8_t m[40 + 4], u[20], blk[64]; uint32_t h[5]; (void)kpad;
    sha1_t c; memcpy(c.h, ist, 20); c.total = 64; c.fill = 0;
    memcpy(m, salt, sl); m[sl] = (uint8_t)(idx >> 24); m[sl + 1] = (uint8_t)(idx >> 16); m[sl + 2] = (uint8_t)(idx >> 8); m[sl + 3] = (uint8_t)idx;
    s_update(&c, m, sl + 4); s_final(&c, u);
    sha1_t o; memcpy(o.h, ost, 20); o.total = 64; o.fill = 0; s_update(&o, u, 20); s_final(&o, u);
    memcpy(t, u, 20);
#ifdef WPA_HW_SHA
    /* the SHA engine, plain API: HMAC = H(opad | H(ipad | u)), four blocks per iteration */
    sha_ctx_t w; uint8_t ip[64], op[64];
    for (int i = 0; i < 64; i++) { ip[i] = kpad[i] ^ 0x36; op[i] = kpad[i] ^ 0x5c; }
    for (unsigned it = 1; it < 4096; it++) {
        sha_start(&w, SHA1); sha_update(&w, ip, 64); sha_update(&w, u, 20); sha_finish(&w, u);
        sha_start(&w, SHA1); sha_update(&w, op, 64); sha_update(&w, u, 20); sha_finish(&w, u);
        for (int i = 0; i < 20; i++) t[i] ^= u[i];
    }
    zero(&w, sizeof w); zero(ip, 64); zero(op, 64);
#else
    for (unsigned it = 1; it < 4096; it++) {
        memset(blk, 0, 64); memcpy(blk, u, 20); blk[20] = 0x80; blk[62] = (84 * 8) >> 8; blk[63] = (84 * 8) & 0xFF;   /* (64 + 20) bytes = 672 bits */
        memcpy(h, ist, 20); sha1_block(h, blk); put(u, h);
        memset(blk, 0, 64); memcpy(blk, u, 20); blk[20] = 0x80; blk[62] = (84 * 8) >> 8; blk[63] = (84 * 8) & 0xFF;
        memcpy(h, ost, 20); sha1_block(h, blk); put(u, h);
        for (int i = 0; i < 20; i++) t[i] ^= u[i];
    }
#endif
    zero(u, sizeof u); zero(blk, sizeof blk); zero(h, sizeof h);
}

void wpa_pmk(const char *pass, const uint8_t *ssid, unsigned ssid_len, uint8_t pmk[32])
{
    uint8_t k[64] = { 0 }, t[64], tb[20], s1[20]; uint32_t ist[5], ost[5]; unsigned pl = (unsigned)strlen(pass);
    if (pl > 63) pl = 63;
    if (ssid_len > 32) ssid_len = 32;
    memcpy(k, pass, pl);
    for (int i = 0; i < 64; i++) t[i] = k[i] ^ 0x36;
    memcpy(ist, iv, 20); sha1_block(ist, t);
    for (int i = 0; i < 64; i++) t[i] = k[i] ^ 0x5c;
    memcpy(ost, iv, 20); sha1_block(ost, t);
    pbkdf2_block(k, ist, ost, ssid, ssid_len, 1, tb); memcpy(pmk, tb, 20);
    pbkdf2_block(k, ist, ost, ssid, ssid_len, 2, s1); memcpy(pmk + 20, s1, 12);
    zero(k, sizeof k); zero(t, sizeof t); zero(tb, sizeof tb); zero(s1, sizeof s1); zero(ist, sizeof ist); zero(ost, sizeof ost);
}

void wpa_ptk(const uint8_t pmk[32], const uint8_t aa[6], const uint8_t spa[6], const uint8_t anonce[32], const uint8_t snonce[32], uint8_t ptk[48])
{
    uint8_t d[22 + 1 + 76 + 1], o[20]; const char *a = "Pairwise key expansion";
    memcpy(d, a, 22); d[22] = 0;
    int c1 = memcmp(aa, spa, 6) < 0, c2 = memcmp(anonce, snonce, 32) < 0;
    memcpy(d + 23, c1 ? aa : spa, 6); memcpy(d + 29, c1 ? spa : aa, 6);
    memcpy(d + 35, c2 ? anonce : snonce, 32); memcpy(d + 67, c2 ? snonce : anonce, 32);
    for (int i = 0; i < 3; i++) { d[99] = (uint8_t)i; wpa_hmac_sha1(pmk, 32, d, 100, o); memcpy(ptk + 20 * i, o, i < 2 ? 20 : 8); }
    zero(o, sizeof o);
}
