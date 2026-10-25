/* Host check that lib/ecc.c p256_verify accepts tools/ota_sign.py signatures and rejects any changed byte.
 *   test_ota_sig PUBHEX DIGESTHEX SIGHEX */
#include "p256.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
void rng_fill(void *b, size_t n) { for (size_t i = 0; i < n; i++) ((uint8_t *)b)[i] = rand(); }
static void hex(const char *h, uint8_t *o, int n) { for (int i = 0; i < n; i++) sscanf(h + 2 * i, "%2hhx", &o[i]); }
int main(int argc, char **argv)
{
    uint8_t pub[64], d[32], sig[64]; if (argc < 4) return 2;
    hex(argv[1], pub, 64); hex(argv[2], d, 32); hex(argv[3], sig, 64);
    int ok = p256_verify(pub, d, sig) == 0, rej = 0;
    for (int i = 0; i < 64; i++) { sig[i] ^= 1; rej += p256_verify(pub, d, sig) != 0; sig[i] ^= 1; }
    for (int i = 0; i < 32; i++) { d[i] ^= 0x80; rej += p256_verify(pub, d, sig) != 0; d[i] ^= 0x80; }
    printf("valid signature %s, %d/96 single-bit changes rejected\n", ok ? "accepted" : "REJECTED", rej);
    return ok && rej == 96 ? 0 : 1;
}
