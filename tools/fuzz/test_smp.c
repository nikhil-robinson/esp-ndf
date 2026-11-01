/* Host test of the Security Manager crypto toolbox (components/ble_host/smp_crypto.c) against the sample data of the Core specification, vol 3 part H appendix D, and RFC 4493.
 * AES comes from OpenSSL here (the target uses the AES accelerator). */
#include "smp.h"
#include <openssl/aes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

void aes_block(const uint8_t *key, int kl, bool enc, const uint8_t in[16], uint8_t out[16])
{
    AES_KEY k; (void)kl; if (enc) AES_set_encrypt_key(key, 128, &k); else AES_set_decrypt_key(key, 128, &k);
    uint8_t t[16]; if (enc) AES_encrypt(in, t, &k); else AES_decrypt(in, t, &k); memcpy(out, t, 16);
}

static int fails;
static unsigned hex(const char *s, uint8_t *o) { unsigned n = 0; for (; s[0] && s[1]; s += 2, n++) { char b[3] = { s[0], s[1], 0 }; o[n] = (uint8_t)strtoul(b, 0, 16); } return n; }
static void be(const char *s, uint8_t *o) { uint8_t t[64]; unsigned n = hex(s, t); for (unsigned i = 0; i < n; i++) o[i] = t[n - 1 - i]; }     /* the spec's most-significant-first hex to our LSO-first array */
static void check(const char *name, const uint8_t *got, const char *want_be, unsigned n)
{
    uint8_t w[64]; be(want_be, w);
    if (memcmp(got, w, n)) { fails++; printf("FAIL %s: got", name); for (int i = (int)n - 1; i >= 0; i--) printf("%02x", got[i]); printf(" want %s\n", want_be); }
}

int main(void)
{
    uint8_t key[16], msg[64], out[16];
    hex("2b7e151628aed2a6abf7158809cf4f3c", key);                                                     /* RFC 4493 */
    smp_cmac(key, msg, 0, out);
    { uint8_t w[16]; hex("bb1d6929e95937287fa37d129b756746", w); if (memcmp(out, w, 16)) { fails++; puts("FAIL cmac empty"); } }
    hex("6bc1bee22e409f96e93d7e117393172a", msg); smp_cmac(key, msg, 16, out);
    { uint8_t w[16]; hex("070a16b46b4d4144f79bdd9dd04a287c", w); if (memcmp(out, w, 16)) { fails++; puts("FAIL cmac 16"); } }
    hex("6bc1bee22e409f96e93d7e117393172aae2d8a571e03ac9c9eb76fac45af8e5130c81c46a35ce411", msg); smp_cmac(key, msg, 40, out);
    { uint8_t w[16]; hex("dfa66747de9ae63030ca32611497c827", w); if (memcmp(out, w, 16)) { fails++; puts("FAIL cmac 40"); } }

    uint8_t u[32], v[32], x[16], y[16], z0 = 0;
    be("20b003d2f297be2c5e2c83a7e9f9a5b9eff49111acf4fddbcc0301480e359de6", u);
    be("55188b3d32f6bb9a900afcfbeed4e72a59cb9ac2f19d7cfb6b4fdd49f47fc5fd", v);
    be("d5cb8454d177733effffb2ec712baeab", x);
    smp_f4(u, v, x, z0, out); check("f4", out, "f2c916f107a9bd1cf1eda1bea974872d", 16);
    be("a6e8e7cc25a75f6e216583f7ff3dc4cf", y);
    { uint32_t g = smp_g2(u, v, x, y); if (g != 0x2f9ed5ba) { fails++; printf("FAIL g2: got %08x\n", g); } }

    uint8_t w[32], n1[16], n2[16], a1[7], a2[7], mk[16], ltk[16];
    be("ec0234a357c8ad05341010a60a397d9b99796b13b4f866f1868d34f373bfa698", w);
    be("d5cb8454d177733effffb2ec712baeab", n1); be("a6e8e7cc25a75f6e216583f7ff3dc4cf", n2);
    a1[0] = 0; be("56123737bfce", a1 + 1); a2[0] = 0; be("a713702dcfc1", a2 + 1);
    smp_f5(w, n1, n2, a1, a2, mk, ltk);
    check("f5 MacKey", mk, "2965f176a1084a02fd3f6a20ce636e20", 16); check("f5 LTK", ltk, "6986791169d7cd23980522b594750a38", 16);
    uint8_t r[16], io[3], mkb[16];
    be("12a3343bb453bb5408da42d20c2d0fc8", r);
    be("010102", io); memcpy(mkb, mk, 16);
    smp_f6(mkb, n1, n2, r, io, a1, a2, out); check("f6", out, "e3c473989cd0e8c5d26c0b09da958f61", 16);

    uint8_t irk[16], pr[3], h[3];
    be("ec0234a357c8ad05341010a60a397d9b", irk); be("708194", pr);
    smp_ah(irk, pr, h); check("ah", h, "0dfbaa", 3);
    puts(fails ? "FAIL smp crypto" : "smp crypto vectors ok"); return fails != 0;
}
