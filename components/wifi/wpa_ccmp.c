/* CCMP (802.11i, AES-CCM with an 8-byte MIC) and RFC 3394 key unwrap on the AES engine's block primitive. */
#include <string.h>
#include "wpa.h"
#include "aes.h"

static void zero(void *p, unsigned n) { volatile uint8_t *q = p; while (n--) *q++ = 0; }
static void E(const uint8_t k[16], const uint8_t in[16], uint8_t out[16]) { aes_block(k, 16, true, in, out); }

/* AAD (802.11i 8.3.3.3.2) and nonce from the MAC header */
static unsigned build_aad(const uint8_t *h, unsigned hl, uint8_t *aad, uint8_t *tid)
{
    unsigned n = 0, qos = (h[0] & 0x80) != 0 && (h[0] & 0x0C) == 0x08, four = (h[1] & 3) == 3;
    aad[n++] = (h[0] & 0x0C) ? h[0] & 0x8F : h[0]; aad[n++] = (h[1] & 0xC7) | 0x40;   /* FC: subtype bits (data frames only: a management frame keeps them, 802.11w) and retry / power / more-data cleared, protected set */
    memcpy(aad + n, h + 4, 18); n += 18;                                              /* A1 A2 A3 */
    aad[n++] = h[22] & 0x0F; aad[n++] = 0;                                            /* sequence control: fragment number only */
    if (four) { memcpy(aad + n, h + 24, 6); n += 6; }
    *tid = 0;
    if (qos) { unsigned q = 24 + (four ? 6 : 0); if (hl < q + 2) return 0; aad[n++] = h[q] & 0x0F; aad[n++] = 0; *tid = h[q] & 0x0F; }
    return n;
}

/* CCM with M = 8, L = 2: CBC-MAC over B0 | AAD | data, then CTR */
static void ccm(const uint8_t k[16], const uint8_t nonce[13], const uint8_t *aad, unsigned al, uint8_t *data, unsigned n, int enc, uint8_t mic[8])
{
    uint8_t x[16], b[16], s[16], a[16];
    b[0] = 0x59; memcpy(b + 1, nonce, 13); b[14] = (uint8_t)(n >> 8); b[15] = (uint8_t)n;
    E(k, b, x);
    unsigned left = al, off = 0, first = 1;
    while (left || first) {
        memset(b, 0, 16); unsigned c = 0;
        if (first) { b[0] = (uint8_t)(al >> 8); b[1] = (uint8_t)al; c = 2; first = 0; }
        unsigned m = 16 - c; if (m > left) m = left;
        memcpy(b + c, aad + off, m); off += m; left -= m;
        for (int i = 0; i < 16; i++) b[i] ^= x[i];
        E(k, b, x);
    }
    a[0] = 0x01; memcpy(a + 1, nonce, 13);
    for (unsigned i = 0; i < n; i += 16) {
        unsigned m = n - i < 16 ? n - i : 16;
        if (!enc) { a[14] = (uint8_t)(((i / 16) + 1) >> 8); a[15] = (uint8_t)((i / 16) + 1); E(k, a, s); for (unsigned j = 0; j < m; j++) data[i + j] ^= s[j]; }
        memset(b, 0, 16); memcpy(b, data + i, m);
        for (int j = 0; j < 16; j++) b[j] ^= x[j];
        E(k, b, x);
        if (enc) { a[14] = (uint8_t)(((i / 16) + 1) >> 8); a[15] = (uint8_t)((i / 16) + 1); E(k, a, s); for (unsigned j = 0; j < m; j++) data[i + j] ^= s[j]; }
    }
    a[14] = a[15] = 0; E(k, a, s);
    for (int i = 0; i < 8; i++) mic[i] = x[i] ^ s[i];
    zero(x, 16); zero(s, 16);
}

static void nonce_of(uint8_t nonce[13], uint8_t tid, const uint8_t *h, uint64_t pn)
{
    nonce[0] = (uint8_t)(tid | ((h[0] & 0x0C) ? 0 : 0x10)); memcpy(nonce + 1, h + 10, 6);   /* nonce flags: priority, Management bit for a management frame (PMF) */
    for (int i = 0; i < 6; i++) nonce[7 + i] = (uint8_t)(pn >> (40 - 8 * i));
}

int ccmp_encrypt(const uint8_t tk[16], const uint8_t *hdr, unsigned hl, uint64_t pn, uint8_t key_id, uint8_t *body, unsigned n)
{
    uint8_t aad[32], tid, nonce[13];
    unsigned al = build_aad(hdr, hl, aad, &tid); if (!al) return -1;
    nonce_of(nonce, tid, hdr, pn);
    body[0] = (uint8_t)pn; body[1] = (uint8_t)(pn >> 8); body[2] = 0; body[3] = (uint8_t)(0x20 | key_id << 6);
    body[4] = (uint8_t)(pn >> 16); body[5] = (uint8_t)(pn >> 24); body[6] = (uint8_t)(pn >> 32); body[7] = (uint8_t)(pn >> 40);
    ccm(tk, nonce, aad, al, body + 8, n, 1, body + 8 + n);
    return (int)n + 16;
}

int ccmp_decrypt(const uint8_t tk[16], const uint8_t *hdr, unsigned hl, uint8_t *body, unsigned total, uint64_t *pn_out)
{
    if (total < 16 || !(body[3] & 0x20)) return -1;                                   /* header and MIC must fit; ExtIV bit must be set */
    uint64_t pn = (uint64_t)body[0] | (uint64_t)body[1] << 8 | (uint64_t)body[4] << 16 | (uint64_t)body[5] << 24 | (uint64_t)body[6] << 32 | (uint64_t)body[7] << 40;
    uint8_t aad[32], tid, nonce[13], mic[8]; unsigned n = total - 16;
    unsigned al = build_aad(hdr, hl, aad, &tid); if (!al) return -1;
    nonce_of(nonce, tid, hdr, pn);
    ccm(tk, nonce, aad, al, body + 8, n, 0, mic);
    uint8_t d = 0; for (int i = 0; i < 8; i++) d |= mic[i] ^ body[8 + n + i];         /* constant time */
    if (d) { zero(body + 8, n); return -1; }                                          /* a bad MIC leaves no plaintext behind */
    if (pn_out) *pn_out = pn;
    return (int)n;
}

int aes_unwrap(const uint8_t kek[16], const uint8_t *in, unsigned n, uint8_t *out)
{
    if (n < 24 || n % 8 || n > 8 + WPA_UNWRAP_MAX) return -1;
    unsigned nb = n / 8 - 1; uint8_t a[8], r[WPA_UNWRAP_MAX], b[16], o[16];
    memcpy(a, in, 8); memcpy(r, in + 8, nb * 8);
    for (int j = 5; j >= 0; j--)
        for (int i = (int)nb; i >= 1; i--) {
            uint32_t t = (uint32_t)(nb * (unsigned)j + (unsigned)i);
            memcpy(b, a, 8); b[7] ^= (uint8_t)t; b[6] ^= (uint8_t)(t >> 8); memcpy(b + 8, r + 8 * (i - 1), 8);
            aes_block(kek, 16, false, b, o);
            memcpy(a, o, 8); memcpy(r + 8 * (i - 1), o + 8, 8);
        }
    uint8_t d = 0; for (int i = 0; i < 8; i++) d |= a[i] ^ 0xA6;
    if (d) { zero(r, sizeof r); return -1; }
    memcpy(out, r, nb * 8); zero(r, sizeof r); zero(b, 16); zero(o, 16);
    return (int)(nb * 8);
}

int aes_wrap(const uint8_t kek[16], const uint8_t *in, unsigned n, uint8_t *out)       /* RFC 3394; n = key length (multiple of 8, 16..64); writes n + 8 bytes */
{
    if (n < 16 || n % 8 || n > 64) return -1;
    unsigned nb = n / 8; uint8_t a[8], r[64], b[16], o[16];
    memset(a, 0xA6, 8); memcpy(r, in, n);
    for (unsigned j = 0; j < 6; j++)
        for (unsigned i = 1; i <= nb; i++) {
            memcpy(b, a, 8); memcpy(b + 8, r + 8 * (i - 1), 8);
            aes_block(kek, 16, true, b, o);
            uint32_t t = nb * j + i;
            memcpy(a, o, 8); a[7] ^= (uint8_t)t; a[6] ^= (uint8_t)(t >> 8); memcpy(r + 8 * (i - 1), o + 8, 8);
        }
    memcpy(out, a, 8); memcpy(out + 8, r, n); zero(r, sizeof r); zero(b, 16); zero(o, 16);
    return (int)(n + 8);
}
