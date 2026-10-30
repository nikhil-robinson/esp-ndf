/* WebSocket frame codec (RFC 6455 5.2) and the handshake key: pure (sha() for SHA-1), host tests and fuzzer drive it. */
#include <string.h>
#include "ws.h"
#include "sha.h"

int ws_frame_parse(const uint8_t *p, unsigned n, ws_hdr_t *h)
{
    if (n < 2) return 0;
    if (p[0] & 0x70) return -1;                                             /* RSV bits: no extension negotiated */
    h->fin = p[0] >> 7; h->op = p[0] & 15; h->masked = p[1] >> 7;
    unsigned hl = 2; uint64_t l = p[1] & 127;
    if (h->op >= 3 && h->op <= 7) return -1;                                /* reserved data opcodes */
    if (h->op > 10) return -1;                                              /* reserved control opcodes */
    if (l == 126) { if (n < 4) return 0; l = (uint64_t)p[2] << 8 | p[3]; hl = 4; if (l < 126) return -1; }   /* minimal length encoding */
    else if (l == 127) { if (n < 10) return 0; l = 0; for (int i = 0; i < 8; i++) l = l << 8 | p[2 + i]; hl = 10; if (l >> 63 || l < 65536) return -1; }
    if (h->op >= 8 && (!h->fin || l > 125)) return -1;                      /* control frames: one frame, at most 125 bytes */
    if (h->masked) { if (n < hl + 4) return 0; memcpy(h->mask, p + hl, 4); hl += 4; }
    h->len = l;
    return (int)hl;
}
unsigned ws_frame_head(uint8_t *o, int fin, int op, uint64_t len, const uint8_t *mask)
{
    unsigned n = 0; o[n++] = (uint8_t)((fin ? 0x80 : 0) | (op & 15)); uint8_t m = mask ? 0x80 : 0;
    if (len < 126) o[n++] = (uint8_t)(m | len);
    else if (len < 65536) { o[n++] = m | 126; o[n++] = (uint8_t)(len >> 8); o[n++] = (uint8_t)len; }
    else { o[n++] = m | 127; for (int i = 7; i >= 0; i--) o[n++] = (uint8_t)(len >> (8 * i)); }
    if (mask) { memcpy(o + n, mask, 4); n += 4; }
    return n;
}
void ws_mask(uint8_t *p, unsigned n, const uint8_t mask[4], unsigned off) { for (unsigned i = 0; i < n; i++) p[i] ^= mask[(off + i) & 3]; }
void ws_accept_key(const char *key, char out[29])
{
    static const char guid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11", b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    uint8_t in[64 + sizeof guid], d[21] = { 0 }; unsigned kl = (unsigned)strnlen(key, 64);
    memcpy(in, key, kl); memcpy(in + kl, guid, sizeof guid - 1);
    sha(SHA1, in, kl + sizeof guid - 1, d);
    unsigned o = 0;
    for (unsigned i = 0; i < 21; i += 3) {                                  /* 20 bytes -> 28 characters, the last one '=' */
        uint32_t v = (uint32_t)d[i] << 16 | (uint32_t)d[i + 1] << 8 | d[i + 2];
        out[o++] = b64[v >> 18 & 63]; out[o++] = b64[v >> 12 & 63]; out[o++] = b64[v >> 6 & 63]; out[o++] = b64[v & 63];
    }
    out[27] = '='; out[28] = 0;
}

