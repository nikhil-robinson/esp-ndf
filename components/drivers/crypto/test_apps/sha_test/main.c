/* SHA accelerator test and benchmark (sibling: bench/idf_sha). NIST vectors for every mode, split/unaligned updates,
 * two interleaved contexts, speed for 64 B .. 64 KB. */
#include "console.h"
#include "sha.h"
#include "cpu.h"
#include <string.h>
#include <stdlib.h>
#include "sys.h"

static uint8_t big[65536 + 4];
static int fail;

static int hexeq(const uint8_t *d, size_t n, const char *hex)
{
    for (size_t i = 0; i < n; i++) { unsigned v; char b[3] = { hex[2 * i], hex[2 * i + 1], 0 };
        v = (unsigned)strtoul(b, 0, 16); if (d[i] != v) return 0; }
    return 1;
}
static void kat(const char *name, sha_type_t t, const void *m, size_t n, const char *hex)
{
    uint8_t d[64]; sha(t, m, n, d);
    int ok = hexeq(d, sha_size(t), hex);
    printf("%s %s\n", ok ? "PASS" : "FAIL", name); fail |= !ok;
}

int main(void)
{
    delay_ms(300);
    const char *abc = "abc", *m448 = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    kat("SHA-1 abc", SHA1, abc, 3, "a9993e364706816aba3e25717850c26c9cd0d89d");
    kat("SHA-224 abc", SHA224, abc, 3, "23097d223405d8228642a477bda255b32aadbce4bda0b3f7e36c9da7");
    kat("SHA-256 abc", SHA256, abc, 3, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    kat("SHA-384 abc", SHA384, abc, 3, "cb00753f45a35e8bb5a03d699ac65007272c32ab0eded1631a8b605a43ff5bed8086072ba1e7cc2358baeca134c825a7");
    kat("SHA-512 abc", SHA512, abc, 3, "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f");
    kat("SHA-512/224 abc", SHA512_224, abc, 3, "4634270f707b6a54daae7530460842e20e37ed265ceee9a43e8924aa");
    kat("SHA-512/256 abc", SHA512_256, abc, 3, "53048e2681941ef99b2e29b76b4c7dabe4c2d0c634fc6d46e0e2f13107e7af23");
    kat("SHA-256 empty", SHA256, "", 0, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    kat("SHA-256 448-bit", SHA256, m448, 56, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    kat("SHA-1 448-bit", SHA1, m448, 56, "84983e441c3bd26ebaae4aa1f95129e5e54670f1");
    memset(big, 'a', 1000);
    { sha_ctx_t c; uint8_t d[32]; sha_start(&c, SHA256); for (int i = 0; i < 1000; i++) sha_update(&c, big, 1000);
      sha_finish(&c, d); int ok = hexeq(d, 32, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
      printf("%s SHA-256 1M 'a' (1000 x 1000-byte updates)\n", ok ? "PASS" : "FAIL"); fail |= !ok; }
    { sha_ctx_t c; uint8_t d[64]; sha_start(&c, SHA512); for (int i = 0; i < 1000; i++) sha_update(&c, big, 1000);
      sha_finish(&c, d); int ok = hexeq(d, 64, "e718483d0ce769644e2e42c7bc15b4638e1f98b13b2044285632a803afa973ebde0ff244877ea60a4cb0432ce577c31beb009c5c2c49aa2e4eadb217ad8cc09b");
      printf("%s SHA-512 1M 'a'\n", ok ? "PASS" : "FAIL"); fail |= !ok; }
    for (size_t i = 0; i < sizeof big; i++) big[i] = (uint8_t)(i * 7 + (i >> 9));
    uint8_t ref[32], d1[32], d2[32];
    sha(SHA256, big, 10000, ref);
    { sha_ctx_t a, b; sha_start(&a, SHA256); sha_start(&b, SHA256);       /* interleaved, odd splits, unaligned */
      size_t o = 0; while (o < 10000) { size_t n = 1 + (o * 13) % 301; if (o + n > 10000) n = 10000 - o;
        sha_update(&a, big + o, n); sha_update(&b, big + o, n); o += n; }
      sha_finish(&a, d1); sha_finish(&b, d2);
      uint8_t d3[32]; memmove(big + 1, big, 10000); sha(SHA256, big + 1, 10000, d3); memmove(big, big + 1, 10000);
      int ok = !memcmp(d1, ref, 32) && !memcmp(d2, ref, 32) && !memcmp(d3, ref, 32);
      printf("%s split / interleaved / unaligned updates match one-shot\n", ok ? "PASS" : "FAIL"); fail |= !ok; }
    static const size_t sz[] = { 64, 1024, 4096, 65536 };
    for (int t = 0; t < 2; t++)
        for (int i = 0; i < 4; i++) {
            uint8_t d[64]; uint32_t best = ~0u;
            for (int r = 0; r < 4; r++) { uint32_t c = ccount(); sha(t ? SHA512 : SHA256, big, sz[i], d); c = ccount() - c; if (c < best) best = c; }
            printf("speed %s %6u B: %7lu cycles (%lu us, %lu MB/s)\n", t ? "SHA-512" : "SHA-256", (unsigned)sz[i],
                   (unsigned long)best, (unsigned long)(best / (sys_cpu_hz() / 1000000)), (unsigned long)((uint64_t)sz[i] * (sys_cpu_hz() / 1000000) / best));
        }
    printf("sha_test %s done\n", fail ? "FAIL" : "PASS");
    for (;;) {}
}
