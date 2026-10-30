/* SHA-256 / AES-CMAC key hierarchy for PMF and WPA3 (IEEE 802.11-2020 12.7.1.6.2 KDF, 12.7.1.7.2 PTK with KDF-SHA256, 12.7.2 AES-128-CMAC MIC,
 * 12.5.4 BIP-CMAC-128). SHA through sha_*() and AES through aes_block() (the host tests provide software versions). */
#include <string.h>
#include "wpa.h"
#include "sha.h"
#include "aes.h"

static void wipe(void *p, unsigned n) { volatile uint8_t *q = p; while (n--) *q++ = 0; }

void wpa_hmac_sha256_v(const uint8_t *key, unsigned kl, unsigned cnt, const uint8_t *const *p, const unsigned *l, uint8_t out[32])
{
    uint8_t k[64] = { 0 }, pad[64], in[32]; sha_ctx_t c;
    if (kl > 64) sha(SHA256, key, kl, k); else memcpy(k, key, kl);
    for (int i = 0; i < 64; i++) pad[i] = k[i] ^ 0x36;
    sha_start(&c, SHA256); sha_update(&c, pad, 64); for (unsigned i = 0; i < cnt; i++) if (l[i]) sha_update(&c, p[i], l[i]); sha_finish(&c, in);
    for (int i = 0; i < 64; i++) pad[i] = k[i] ^ 0x5c;
    sha_start(&c, SHA256); sha_update(&c, pad, 64); sha_update(&c, in, 32); sha_finish(&c, out);
    wipe(k, sizeof k); wipe(pad, sizeof pad); wipe(in, sizeof in);
}
void wpa_hmac_sha256(const uint8_t *key, unsigned kl, const uint8_t *d, unsigned n, uint8_t out[32]) { wpa_hmac_sha256_v(key, kl, 1, &d, &n, out); }

/* KDF-Hash-Length: HMAC-SHA256(K, i || label || context || length), i and length (bits) little-endian 16-bit */
void wpa_kdf_sha256(const uint8_t *key, unsigned kl, const char *label, const uint8_t *ctx, unsigned cl, uint8_t *out, unsigned bits)
{
    uint8_t i_le[2], len_le[2] = { (uint8_t)bits, (uint8_t)(bits >> 8) }, b[32];
    const uint8_t *p[4] = { i_le, (const uint8_t *)label, ctx, len_le }; unsigned l[4] = { 2, (unsigned)strlen(label), cl, 2 };
    for (unsigned i = 1, done = 0; done < bits / 8; i++) {
        i_le[0] = (uint8_t)i; i_le[1] = (uint8_t)(i >> 8);
        wpa_hmac_sha256_v(key, kl, 4, p, l, b);
        unsigned m = bits / 8 - done < 32 ? bits / 8 - done : 32; memcpy(out + done, b, m); done += m;
    }
    wipe(b, sizeof b);
}

void wpa_ptk_sha256(const uint8_t pmk[32], const uint8_t aa[6], const uint8_t spa[6], const uint8_t anonce[32], const uint8_t snonce[32], uint8_t ptk[48])
{
    uint8_t c[76]; int a = memcmp(aa, spa, 6) < 0, n = memcmp(anonce, snonce, 32) < 0;
    memcpy(c, a ? aa : spa, 6); memcpy(c + 6, a ? spa : aa, 6); memcpy(c + 12, n ? anonce : snonce, 32); memcpy(c + 44, n ? snonce : anonce, 32);
    wpa_kdf_sha256(pmk, 32, "Pairwise key expansion", c, sizeof c, ptk, 384);
}

/* AES-CMAC (RFC 4493), over a list of buffers */
static void dbl(uint8_t b[16]) { uint8_t c = b[0] >> 7; for (int i = 0; i < 15; i++) b[i] = (uint8_t)(b[i] << 1 | b[i + 1] >> 7); b[15] = (uint8_t)(b[15] << 1) ^ (c ? 0x87 : 0); }
void wpa_cmac_v(const uint8_t key[16], unsigned cnt, const uint8_t *const *p, const unsigned *l, uint8_t out[16])
{
    uint8_t k1[16] = { 0 }, x[16] = { 0 }, blk[16]; unsigned total = 0, fill = 0, seen = 0;
    aes_block(key, 16, true, k1, k1); dbl(k1);
    for (unsigned i = 0; i < cnt; i++) total += l[i];
    for (unsigned i = 0; i < cnt; i++)
        for (unsigned j = 0; j < l[i]; j++) {
            blk[fill++] = p[i][j]; seen++;
            if (fill == 16 && seen < total) { for (int t = 0; t < 16; t++) x[t] ^= blk[t]; aes_block(key, 16, true, x, x); fill = 0; }
        }
    if (fill == 16 && total) { for (int t = 0; t < 16; t++) blk[t] ^= k1[t]; }               /* complete last block: K1 */
    else { blk[fill++] = 0x80; while (fill < 16) blk[fill++] = 0; dbl(k1); for (int t = 0; t < 16; t++) blk[t] ^= k1[t]; }   /* padded: K2 */
    for (int t = 0; t < 16; t++) x[t] ^= blk[t];
    aes_block(key, 16, true, x, out);
    wipe(k1, sizeof k1); wipe(x, sizeof x); wipe(blk, sizeof blk);
}
void wpa_cmac(const uint8_t key[16], const uint8_t *d, unsigned n, uint8_t out[16]) { wpa_cmac_v(key, 1, &d, &n, out); }

/* BIP-CMAC-128 on a group-addressed management frame ending in the MMIE (id 76, len 16, key id 2, IPN 6, MIC 8). 0 = MIC good; *ipn = its IPN. */
int wpa_bip_check(const uint8_t igtk[16], const uint8_t *f, unsigned n, uint64_t *ipn)
{
    if (n < 24 + 18 || f[n - 18] != 76 || f[n - 17] != 16) return -1;
    uint8_t aad[20], zero[8] = { 0 }, mic[16];
    aad[0] = f[0]; aad[1] = f[1] & (uint8_t)~0x38; memcpy(aad + 2, f + 4, 18);                 /* FC with retry / power management / more data cleared, A1 A2 A3 */
    const uint8_t *p[4] = { aad, f + 24, zero }; unsigned l[4] = { 20, n - 24 - 8, 8 };
    wpa_cmac_v(igtk, 3, p, l, mic);
    uint8_t d = 0; for (int i = 0; i < 8; i++) d |= mic[i] ^ f[n - 8 + i];
    *ipn = 0; for (int i = 5; i >= 0; i--) *ipn = *ipn << 8 | f[n - 14 + i];
    return d ? -1 : 0;
}

/* EAPOL-Key MIC with AES-128-CMAC (key descriptor version 3, and the AKM-defined version 0 of SAE): 0 = good, constant time */
int eapol_key_check_cmac(const uint8_t kck[16], const uint8_t *frame, unsigned n)
{
    eapol_key_t k; uint8_t cp[512], out[16];
    if (eapol_key_parse(frame, n, &k)) return -1;
    unsigned len = 4 + (unsigned)(frame[2] << 8 | frame[3]);
    if (len > sizeof cp) return -1;
    memcpy(cp, frame, len); memset(cp + 81, 0, 16);
    wpa_cmac(kck, cp, len, out);
    uint8_t d = 0; for (int i = 0; i < 16; i++) d |= out[i] ^ frame[81 + i];
    return d ? -1 : 0;
}
