/* Security Manager responder (components/ble_host/smp.c) under ASan / UBSan: a complete LE Secure Connections Just Works pairing against an initiator built from the same toolbox,
 * the bond store and its lookup (also through a resolvable private address), a tampered DHKey check, out-of-order and wrong-length PDUs, then random PDUs. Nothing may crash,
 * no key may be accepted from a failed pairing. usage: fuzz_smp <rounds> <seed> */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "smp.h"
#include "p256.h"
#include "../tls_shims.c"

void aes_block(const uint8_t *key, int kl, bool enc, const uint8_t in[16], uint8_t out[16])
{
    EVP_CIPHER_CTX *c = EVP_CIPHER_CTX_new(); int n;
    EVP_CipherInit_ex(c, kl == 32 ? EVP_aes_256_ecb() : EVP_aes_128_ecb(), 0, key, 0, enc); EVP_CIPHER_CTX_set_padding(c, 0);
    EVP_CipherUpdate(c, out, &n, in, 16); EVP_CIPHER_CTX_free(c);
}

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)
static uint8_t out[8][80]; static unsigned outn[8], nout;
static int sink(const uint8_t *p, unsigned n) { if (nout < 8) { memcpy(out[nout], p, n); outn[nout++] = n; } return 0; }
static uint8_t saved[1024]; static unsigned saved_n;
static int save(const void *b, unsigned n) { memcpy(saved, b, n); saved_n = n; return 0; }
static int load(void *b, unsigned *n) { if (!saved_n) return -1; memcpy(b, saved, saved_n); *n = saved_n; return 0; }
static void rev(uint8_t *o, const uint8_t *i, unsigned n) { for (unsigned k = 0; k < n; k++) o[k] = i[n - 1 - k]; }

static const uint8_t peer[7] = { 1, 0x11, 0x22, 0x33, 0x44, 0x55, 0xC6 }, own[7] = { 1, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xC0 };

/* one pairing as the initiator; returns the LTK both sides derived through *ltk (0 on success) */
static int pair(int tamper, uint8_t ltk[16], uint8_t irk_a[16])
{
    uint8_t req[7] = { 1, 3, 0, 0x09, 16, 3, 3 }, pk[64], priv[32], pkbe[64], pkb[64], na[16], nb[16], dh[32], dhle[32], mk[16], l[16], ea[16], eb[16], zero[16] = { 0 };
    nout = 0; smp_rx(req, 7);
    CHECK(nout == 1 && outn[0] == 7 && out[0][0] == 2 && out[0][3] == 0x09 && out[0][4] == 16);
    uint8_t rsp[7]; memcpy(rsp, out[0], 7);
    p256_keygen(priv, pkbe); rev(pk, pkbe, 32); rev(pk + 32, pkbe + 32, 32);
    uint8_t m[65] = { 0x0C }; memcpy(m + 1, pk, 64); nout = 0; smp_rx(m, 65);
    CHECK(nout == 2 && outn[0] == 65 && out[0][0] == 0x0C && outn[1] == 17 && out[1][0] == 3);
    memcpy(pkb, out[0] + 1, 64); uint8_t cb[16]; memcpy(cb, out[1] + 1, 16);
    uint8_t pb_be[64]; rev(pb_be, pkb, 32); rev(pb_be + 32, pkb + 32, 32);
    CHECK(p256_ecdh(dh, priv, pb_be) == 0); rev(dhle, dh, 32);
    rng_fill(na, 16); uint8_t r[17] = { 4 }; memcpy(r + 1, na, 16); nout = 0; smp_rx(r, 17);
    CHECK(nout == 1 && outn[0] == 17 && out[0][0] == 4); memcpy(nb, out[0] + 1, 16);
    uint8_t cb2[16]; smp_f4(pkb, pk, nb, 0, cb2); CHECK(!memcmp(cb, cb2, 16));
    smp_f5(dhle, na, nb, peer, own, mk, l);
    smp_f6(mk, na, nb, zero, req + 1, peer, own, ea); if (tamper) ea[3] ^= 1;
    uint8_t c[17] = { 0x0D }; memcpy(c + 1, ea, 16); nout = 0; smp_rx(c, 17);
    if (tamper) { CHECK(nout == 1 && out[0][0] == 5 && out[0][1] == 0x0B); uint8_t z[8] = { 0 }; uint8_t k[16]; CHECK(smp_ltk(z, 0, k) != 0); return -1; }
    CHECK(nout == 1 && out[0][0] == 0x0D);
    smp_f6(mk, nb, na, zero, rsp + 1, own, peer, eb); CHECK(!memcmp(eb, out[0] + 1, 16));
    memcpy(ltk, l, 16);
    uint8_t z[8] = { 0 }, k[16]; CHECK(smp_ltk(z, 0, k) == 0 && !memcmp(k, l, 16));
    uint8_t one[8] = { 1 }; CHECK(smp_ltk(one, 0, k) != 0); CHECK(smp_ltk(z, 5, k) != 0);
    nout = 0; smp_encrypted(1);                                                      /* the responder distributes its identity first */
    CHECK(nout == 2 && out[0][0] == 8 && outn[0] == 17 && out[1][0] == 9 && outn[1] == 8 && !memcmp(out[1] + 1, own, 7));
    uint8_t id[17] = { 8 }; rng_fill(irk_a, 16); memcpy(id + 1, irk_a, 16); smp_rx(id, 17);
    uint8_t ad[8] = { 9 }; memcpy(ad + 1, peer, 7); smp_rx(ad, 8);
    return 0;
}

int main(int argc, char **argv)
{
    unsigned rounds = argc > 1 ? (unsigned)atoi(argv[1]) : 1000; srand(argc > 2 ? (unsigned)atoi(argv[2]) : 1);
    smp_init(sink); smp_set_store(load, save); smp_connected(peer, own);
    uint8_t ltk[16], irk[16];
    CHECK(pair(0, ltk, irk) == 0);
    smp_bond_t b[4]; CHECK(smp_bonds(b, 4) == 1 && !memcmp(b[0].ltk, ltk, 16) && !memcmp(b[0].irk, irk, 16) && b[0].has_irk && !memcmp(b[0].addr, peer, 7));
    smp_disconnected(); smp_connected(peer, own);
    uint8_t z[8] = { 0 }, k[16];
    CHECK(smp_ltk(z, 0, k) == 0 && !memcmp(k, ltk, 16));                              /* reconnect: the bond gives the key */
    uint8_t rpa[7] = { 1, 0, 0, 0, 0, 0, 0x6A }, h[3]; rpa[4] = 0x12; rpa[5] = 0x34; rpa[6] = 0x6A;          /* prand 6a 34 12 (top bits 01) */
    smp_ah(irk, rpa + 4, h); memcpy(rpa + 1, h, 3);
    smp_disconnected(); smp_connected(rpa, own);
    CHECK(smp_ltk(z, 0, k) == 0 && !memcmp(k, ltk, 16));                              /* resolvable private address resolved through the IRK */
    rpa[1] ^= 1; smp_disconnected(); smp_connected(rpa, own); CHECK(smp_ltk(z, 0, k) != 0);
    smp_disconnected(); smp_connected(peer, own);
    uint8_t persisted[1024]; memcpy(persisted, saved, sizeof saved);
    smp_init(sink); smp_set_store(load, save); smp_connected(peer, own);               /* a new boot: the store brings the bond back */
    CHECK(smp_ltk(z, 0, k) == 0 && !memcmp(k, ltk, 16));
    smp_bonds_clear(); CHECK(smp_ltk(z, 0, k) != 0);
    smp_disconnected(); smp_connected(peer, own); uint8_t l2[16], i2[16];
    CHECK(pair(1, l2, i2) != 0);                                                       /* a wrong DHKey check: Pairing Failed, no key */
    /* out of order, wrong lengths, random */
    static const uint8_t ops[] = { 1, 3, 4, 5, 6, 7, 8, 9, 0xA, 0xB, 0xC, 0xD, 0, 0xE, 0xFF };
    for (unsigned i = 0; i < rounds; i++) {
        uint8_t p[80]; unsigned n = (unsigned)rand() % 80; for (unsigned j = 0; j < n; j++) p[j] = (uint8_t)rand();
        if (n && rand() & 1) p[0] = ops[(unsigned)rand() % sizeof ops];
        if (!(rand() % 50)) { smp_disconnected(); smp_connected(peer, own); }
        nout = 0; smp_rx(p, n);
        if (!(rand() % 7)) { nout = 0; smp_encrypted(rand() & 1); }
        smp_ltk(z, 0, k);
    }
    printf("fuzz_smp: %u PDUs%s\n", rounds, fails ? ", FAILED" : ", no crash"); return fails != 0;
}
