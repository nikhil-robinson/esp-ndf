/* WebSocket codec (components/net/ws_frame.c) under ASan / UBSan: RFC 6455 1.3 accept key, header round trips at every length class,
 * reject rules (RSV bits, reserved opcodes, fragmented or long control frames, non-minimal lengths), every truncation, random bytes.
 * usage: fuzz_ws <rounds> <seed> */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <openssl/evp.h>
#include "ws.h"
#include "sha.h"
void sha(sha_type_t t, const void *d, size_t n, uint8_t *o) { (void)t; unsigned l; EVP_Digest(d, n, o, &l, EVP_sha1(), 0); }   /* SHA-1 only here (OpenSSL) */
static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)
int main(int argc, char **argv)
{
    unsigned rounds = argc > 1 ? (unsigned)atoi(argv[1]) : 100000; srand(argc > 2 ? (unsigned)atoi(argv[2]) : 1);
    char acc[29]; ws_accept_key("dGhlIHNhbXBsZSBub25jZQ==", acc); CHECK(!strcmp(acc, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo="));
    static const uint64_t lens[] = { 0, 1, 125, 126, 127, 65535, 65536, 1u << 20 };
    for (unsigned i = 0; i < sizeof lens / sizeof lens[0]; i++) for (int m = 0; m < 2; m++) {
        uint8_t o[14], mk[4] = { 1, 2, 3, 4 }; ws_hdr_t h; unsigned hl = ws_frame_head(o, 1, WS_BIN, lens[i], m ? mk : 0);
        CHECK(ws_frame_parse(o, hl, &h) == (int)hl && h.len == lens[i] && h.op == WS_BIN && h.fin && h.masked == m && (!m || !memcmp(h.mask, mk, 4)));
        for (unsigned c = 0; c < hl; c++) CHECK(ws_frame_parse(o, c, &h) == 0);   /* every truncation: need more */
    }
    ws_hdr_t h; uint8_t f[14];
    f[0] = 0xC1; f[1] = 0; CHECK(ws_frame_parse(f, 2, &h) == -1);                 /* RSV1 */
    f[0] = 0x83; CHECK(ws_frame_parse(f, 2, &h) == -1);                           /* opcode 3 */
    f[0] = 0x8B; CHECK(ws_frame_parse(f, 2, &h) == -1);                           /* opcode 11 */
    f[0] = 0x09; CHECK(ws_frame_parse(f, 2, &h) == -1);                           /* ping without FIN */
    f[0] = 0x89; f[1] = 126; f[2] = 0; f[3] = 200; CHECK(ws_frame_parse(f, 4, &h) == -1);   /* ping with 200 bytes */
    f[0] = 0x82; f[1] = 126; f[2] = 0; f[3] = 100; CHECK(ws_frame_parse(f, 4, &h) == -1);   /* 100 in the 16-bit form */
    f[0] = 0x82; f[1] = 127; memset(f + 2, 0, 8); f[9] = 200; CHECK(ws_frame_parse(f, 10, &h) == -1);   /* 200 in the 64-bit form */
    f[1] = 127; f[2] = 0x80; CHECK(ws_frame_parse(f, 10, &h) == -1);              /* top bit of the 64-bit length */
    uint8_t d[64], k[4] = { 9, 8, 7, 6 }; for (int i = 0; i < 64; i++) d[i] = (uint8_t)i;
    ws_mask(d, 10, k, 0); ws_mask(d + 10, 54, k, 10); ws_mask(d, 64, k, 0); for (int i = 0; i < 64; i++) CHECK(d[i] == i);   /* masking in two pieces = in one */
    for (unsigned r = 0; r < rounds; r++) { uint8_t g[14]; unsigned n = (unsigned)(rand() % 15); for (unsigned i = 0; i < n; i++) g[i] = (uint8_t)rand(); int x = ws_frame_parse(g, n, &h); CHECK(x <= (int)n && x >= -1 && x <= 14); }
    printf("fuzz_ws: %u random headers, %s\n", rounds, fails ? "FAILED" : "no memory error, all rules held");
    return fails ? 1 : 0;
}
