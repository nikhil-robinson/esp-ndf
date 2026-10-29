#include "unit.h"
#include "console.h"
#include "sys.h"
#include "aes.h"
#include "sha.h"
#include "p256.h"
#include "x25519.h"
#include "tls.h"
#include <string.h>
#include "certs.h"

static int hexeq(const uint8_t *got, const char *want) { for (unsigned i = 0; want[i]; i += 2) { unsigned v = 0; for (int k = 0; k < 2; k++) { char c = want[i + k]; v = v * 16 + (unsigned)(c <= '9' ? c - '0' : c - 'a' + 10); } if (got[i / 2] != v) return 0; } return 1; }

TEST(kat)                                               /* the engines' output against published values */
{
    uint8_t o[32], key[32]; memset(key, 0x0b, 20);
    tls_hmac(key, 20, (const uint8_t *)"Hi There", 8, o); UASSERT(hexeq(o, "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7"));
    uint8_t zero[32] = { 0 }, early[32], empty[32], derived[32];
    tls_hkdf_extract(0, 0, zero, 32, early); UASSERT(hexeq(early, "33ad0a1c607ec03b09e6cd9893680ce210adf300aa1f2660e1b22e10f170f92a"));
    sha(SHA256, "", 0, empty); tls_derive_secret(early, "derived", empty, derived); UASSERT(hexeq(derived, "6f2615a108c702c5678f54fc9dbab69716c076189c48250cebeac3576c3611ba"));
    uint8_t k16[16] = { 0 }, iv[12] = { 0 }, tag[16], ct[16] = { 0 };                       /* AES-GCM test case 2 (zero key, zero block) */
    UASSERT(aes_gcm(k16, 16, true, iv, 12, 0, 0, ct, ct, 16, tag) == 0);
    UASSERT(hexeq(ct, "0388dace60b6a392f328c2b971b2fe78") && hexeq(tag, "ab6e47d42cec13bdf53a67b21257bddf"));
}

TEST(x509)
{
    x509_t c;
    UASSERT(x509_parse(tls_server_cert, sizeof tls_server_cert, &c) == 0);
    UASSERT(x509_verify(&c, tls_ca_pub) == 0 && x509_verify(&c, tls_other_ca_pub) != 0);
    UASSERT(x509_host_ok(&c, "ndf.local") == 0 && x509_host_ok(&c, "a.example.test") == 0 && x509_host_ok(&c, "evil.com") != 0 && x509_host_ok(&c, "example.test") != 0);
    for (unsigned n = 0; n < sizeof tls_server_cert; n += 7) UASSERT(x509_parse(tls_server_cert, n, &c) != 0);       /* truncated certificates never parse */
}

static tls_t C, S;
static unsigned long t_c, t_s;                                                           /* microseconds spent inside each engine */
static void move(tls_t *from, tls_t *to, unsigned long *acc)
{
    unsigned n; const uint8_t *o = tls_out(from, &n);
    while (n) {
        uint32_t t0 = time_us(); int u = tls_feed(to, o, n < 1500 ? n : 1500); *acc += (uint32_t)time_us() - t0;
        if (u <= 0) { uint8_t d[4200]; if (tls_read(to, d, sizeof d) <= 0) return; continue; }
        tls_out_done(from, (unsigned)u); o = tls_out(from, &n);
    }
}
TEST(loopback)                                          /* handshake, then 150 KB each way; every byte checked */
{
    static tls_client_cfg_t cc; cc.ca_pub = tls_ca_pub; cc.host = "ndf.local";
    static tls_server_cfg_t sc; sc.cert = tls_server_cert; sc.cert_len = sizeof tls_server_cert; sc.priv = tls_server_priv;
    uint32_t t0 = time_us(); tls_client_init(&C, &cc); t_c = (uint32_t)time_us() - t0; t_s = 0; tls_server_init(&S, &sc);
    for (int i = 0; i < 8 && (tls_state(&C) != TLS_CONNECTED || tls_state(&S) != TLS_CONNECTED); i++) { move(&C, &S, &t_s); move(&S, &C, &t_c); }
    UASSERT(tls_state(&C) == TLS_CONNECTED && tls_state(&S) == TLS_CONNECTED);
    printf("      handshake CPU time: client %lu us (ClientHello + ServerHello .. Finished), server %lu us (X25519 + ECDSA sign + schedule)\n", t_c, t_s);
    unsigned cs = 0, ss = 0, cr = 0, sr = 0; static uint8_t p[1400], d[4200]; int bad = 0; t_c = t_s = 0; uint32_t w0 = time_us();
    while (cr < 150000 || sr < 150000) {
        if (cs < 150000) { for (unsigned i = 0; i < sizeof p; i++) p[i] = (uint8_t)(cs + i); int w = tls_write(&C, p, sizeof p); if (w > 0) cs += (unsigned)w; }
        if (ss < 150000) { for (unsigned i = 0; i < sizeof p; i++) p[i] = (uint8_t)(0x55 + ss + i); int w = tls_write(&S, p, sizeof p); if (w > 0) ss += (unsigned)w; }
        move(&C, &S, &t_s); move(&S, &C, &t_c);
        int n; while ((n = tls_read(&S, d, sizeof d)) > 0) { for (int i = 0; i < n; i++) bad += d[i] != (uint8_t)(sr + i); sr += (unsigned)n; }
        while ((n = tls_read(&C, d, sizeof d)) > 0) { for (int i = 0; i < n; i++) bad += d[i] != (uint8_t)(0x55 + cr + i); cr += (unsigned)n; }
        if (cs >= 150000 && ss >= 150000 && cr == 0 && sr == 0) break;
    }
    uint32_t ms = (uint32_t)(time_us() - w0) / 1000;
    printf("      300 KB through two engines in %lu ms (%lu KB/s, seal + open + copy)\n", (unsigned long)ms, (unsigned long)(300000u / (ms ? ms : 1)));
    UASSERT(bad == 0 && sr >= 150000 && cr >= 150000);
    tls_close(&C); move(&C, &S, &t_s); UASSERT(tls_read(&S, d, sizeof d) == TLS_ERR_CLOSED);
}

TEST(primitives_timing)
{
    uint8_t priv[32], pub[32], sh[32], h[32] = { 1 }, sig[64]; memset(priv, 7, 32);
    uint32_t t = time_us(); x25519_base(pub, priv); uint32_t t1 = (uint32_t)time_us() - t;
    t = time_us(); x25519(sh, priv, pub); uint32_t t2 = (uint32_t)time_us() - t;
    uint8_t ep[64]; p256_pubkey(ep, tls_server_priv);
    t = time_us(); p256_sign(sig, tls_server_priv, h); uint32_t t3 = (uint32_t)time_us() - t;
    t = time_us(); int v = p256_verify(ep, h, sig); uint32_t t4 = (uint32_t)time_us() - t;
    { uint8_t key[16] = { 0 }, iv[12] = { 0 }, tg[16]; static uint8_t buf[4096]; t = time_us();
      for (int i = 0; i < 256; i++) aes_gcm(key, 16, true, iv, 12, 0, 0, buf, buf, sizeof buf, tg);
      printf("      AES-128-GCM 1 MB in 4096-byte calls: %lu us\n", (unsigned long)((uint32_t)time_us() - t)); }
    printf("      X25519 base %lu us, shared %lu us, ECDSA P-256 sign %lu us, verify %lu us\n", (unsigned long)t1, (unsigned long)t2, (unsigned long)t3, (unsigned long)t4);
    UASSERT(v == 0);
}
