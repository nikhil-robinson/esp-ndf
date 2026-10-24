/* AES modes: NIST SP 800-38A F.1-F.5 vectors (AES-128, 4 blocks) by DMA and block by block (CPU path); 64 KB random
 * data DMA vs CPU for every mode, both directions; speed (sibling: bench/idf_aes). */
#include "console.h"
#include "aes.h"
#include "rng.h"
#include "sys.h"
#include "xtensa.h"
#include <string.h>
#include <stdlib.h>

static const uint8_t K[16] = { 0x2b,0x7e,0x15,0x16,0x28,0xae,0xd2,0xa6,0xab,0xf7,0x15,0x88,0x09,0xcf,0x4f,0x3c };
static uint8_t P[64], A[65536], B[65536], C[65536];
static void hex(const char *h, uint8_t *o, int n) { for (int i = 0; i < n; i++) { char b[3] = { h[2*i], h[2*i+1], 0 }; o[i] = (uint8_t)strtoul(b, 0, 16); } }
static int fail;

static void kat(const char *name, int mode, const char *ivh, const char *cth)
{
    uint8_t iv[16], iv2[16], ct[64], o[64], o2[64];
    hex(cth, ct, 64);
    if (ivh) hex(ivh, iv, 16); else memset(iv, 0, 16);
    memcpy(iv2, iv, 16);
    aes_crypt(mode, K, 16, true, iv, P, o, 64);                            /* 64 B: the DMA path */
    for (int j = 0; j < 64; j += 16) aes_crypt(mode, K, 16, true, iv2, P + j, o2 + j, 16);   /* CPU path */
    uint8_t ivd[16], d[64]; if (ivh) hex(ivh, ivd, 16); else memset(ivd, 0, 16);
    aes_crypt(mode, K, 16, false, ivd, ct, d, 64);
    int ok = !memcmp(o, ct, 64) && !memcmp(o2, ct, 64) && !memcmp(d, P, 64);
    printf("%s %s (DMA, per block, decrypt)\n", ok ? "PASS" : "FAIL", name); fail |= !ok;
}

int main(void)
{
    delay_ms(200);
    hex("6bc1bee22e409f96e93d7e117393172aae2d8a571e03ac9c9eb76fac45af8e5130c81c46a35ce411e5fbc1191a0a52eff69f2445df4f9b17ad2b417be66c3710", P, 64);
    kat("ECB-AES128 (F.1.1)", AES_ECB, 0, "3ad77bb40d7a3660a89ecaf32466ef97f5d3d58503b9699de785895a96fdbaaf43b1cd7f598ece23881b00e3ed0306887b0c785e27e8ad3f8223207104725dd4");
    kat("CBC-AES128 (F.2.1)", AES_CBC, "000102030405060708090a0b0c0d0e0f", "7649abac8119b246cee98e9b12e9197d5086cb9b507219ee95db113a917678b273bed6b8e3c1743b7116e69e222295163ff1caa1681fac09120eca307586e1a7");
    kat("CFB128-AES128 (F.3.13)", AES_CFB128, "000102030405060708090a0b0c0d0e0f", "3b3fd92eb72dad20333449f8e83cfb4ac8a64537a0b3a93fcde3cdad9f1ce58b26751f67a3cbb140b1808cf187a4f4dfc04b05357c5d1c0eeac4c66f9ff7f2e6");
    kat("OFB-AES128 (F.4.1)", AES_OFB, "000102030405060708090a0b0c0d0e0f", "3b3fd92eb72dad20333449f8e83cfb4a7789508d16918f03f53c52dac54ed8259740051e9c5fecf64344f7a82260edcc304c6528f659c77866a510d9c1d6ae5e");
    kat("CTR-AES128 (F.5.1)", AES_CTR, "f0f1f2f3f4f5f6f7f8f9fafbfcfdfeff", "874d6191b620e3261bef6864990db6ce9806f66b7970fdff8617187bb9fffdff5ae4df3edbd5d35e5b4f09020db03eab1e031dda2fbe03d1792170a0f3009cee");
    rng_fill(A, sizeof A);
    static const int modes[5] = { AES_ECB, AES_CBC, AES_OFB, AES_CTR, AES_CFB128 }; static const char *mn[5] = { "ECB", "CBC", "OFB", "CTR", "CFB128" };
    uint8_t K2[32]; rng_fill(K2, 32);
    for (int m = 0; m < 5; m++) {
        uint8_t iv[16] = { 1, 2, 3 }, iv2[16] = { 1, 2, 3 }, iv3[16] = { 1, 2, 3 };
        aes_crypt(modes[m], K2, 32, true, iv, A, B, sizeof A);              /* DMA */
        for (size_t j = 0; j < sizeof A; j += 16) aes_crypt(modes[m], K2, 32, true, iv2, A + j, C + j, 16);   /* CPU */
        int same = !memcmp(B, C, sizeof A) && !memcmp(iv, iv2, 16);
        aes_crypt(modes[m], K2, 32, false, iv3, B, C, sizeof A);
        int back = !memcmp(C, A, sizeof A);
        printf("%s AES-256-%s 64 KB: DMA = CPU path %d, decrypts back %d\n", same && back ? "PASS" : "FAIL", mn[m], same, back);
        fail |= !(same && back);
    }
    static const size_t sz[] = { 16, 256, 4096, 65536 };
    for (int m = 1; m <= 3; m += 2) for (int i = 0; i < 4; i++) {
        uint32_t best = ~0u;
        for (int r = 0; r < 4; r++) { uint8_t iv[16] = { 0 }; uint32_t c = ccount(); aes_crypt(m == 1 ? AES_CBC : AES_CTR, K2, 16, true, iv, A, B, sz[i]); c = ccount() - c; if (c < best) best = c; }
        printf("speed AES-128-%s %5u B: %7lu cycles (%lu MB/s)\n", m == 1 ? "CBC" : "CTR", (unsigned)sz[i], (unsigned long)best, (unsigned long)((uint64_t)sz[i] * 240 / best));
    }
    printf("aes_test %s done\n", fail ? "FAIL" : "PASS");
    for (;;) {}
}
