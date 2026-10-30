/* Host test + fuzzer of components/tls: a client and a server engine talk to each other in memory (handshake, 150 KB each way), then the same exchange with
 * random bit flips, truncations and duplicated bytes in the stream, X.509 parsing of truncated / flipped certificates, and garbage into fresh engines.
 * Invariants: no crash or overread (ASan/UBSan); application data that is delivered is exactly what was sent; a flipped byte never ends in a successful
 * handshake with different traffic keys.   usage: fuzz_tls <cert.der> <priv_hex64> <ca_pub_hex128> [rounds] [seed] */
#include "tls.h"
#include "x509.h"
#include "p256.h"
#include "tls_shims.c"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned long x = 1;
static unsigned rnd(void) { x ^= x << 13; x ^= x >> 7; x ^= x << 17; return (unsigned)x; }
static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)
static int unhex(const char *s, uint8_t *o, unsigned n) { for (unsigned i = 0; i < n; i++) { unsigned v; if (sscanf(s + 2 * i, "%2x", &v) != 1) return -1; o[i] = (uint8_t)v; } return 0; }

static tls_t C, S;
static uint8_t cert[3000]; static unsigned cert_len; static uint8_t priv[32], ca[64];

/* move what `from` queued to `to`, optionally mangling it; returns bytes moved */
static unsigned move(tls_t *from, tls_t *to, int mangle)
{
    unsigned n; const uint8_t *o = tls_out(from, &n); if (!n) return 0;
    uint8_t buf[TLS_REC_BUF * 2]; if (n > sizeof buf) n = sizeof buf; memcpy(buf, o, n); tls_out_done(from, n);
    if (mangle) {
        switch (rnd() % 4) {
        case 0: buf[rnd() % n] ^= (uint8_t)(1u << (rnd() % 8)); break;               /* flip a bit */
        case 1: n = rnd() % n; break;                                                   /* truncate */
        case 2: { unsigned p = rnd() % n, l = 1 + rnd() % 16; if (p + l > n) l = n - p; memmove(buf + p + l, buf + p, n - p > sizeof buf - p - l ? 0 : n - p); n = n + l > sizeof buf ? n : n + l; break; }   /* duplicate a few bytes */
        default: for (unsigned k = 0; k < 4; k++) buf[rnd() % n] = (uint8_t)rnd(); break;   /* garble */
        }
    }
    unsigned off = 0;
    while (off < n) { int u = tls_feed(to, buf + off, n - off); if (u <= 0) { if (u < 0) return n; uint8_t d[8192]; if (tls_read(to, d, sizeof d) <= 0) return n; continue; } off += (unsigned)u; }
    return n;
}

/* run a full exchange; returns 1 when both sides finished the data transfer. only_v: the client's only_version (0x0303 = the TLS 1.2 path) */
static uint16_t only_v;
static int exchange(int mangle, unsigned bytes, int *delivered_ok)
{
    static tls_client_cfg_t cfg; cfg.ca_pub = ca; cfg.host = "ndf.local"; cfg.only_version = only_v;
    static tls_server_cfg_t scfg; scfg.cert = cert; scfg.cert_len = cert_len; scfg.priv = priv;
    tls_client_init(&C, &cfg); tls_server_init(&S, &scfg);
    *delivered_ok = 1;
    uint8_t d[8192]; unsigned cs = 0, ss = 0, cr = 0, sr = 0; int idle = 0;
    for (unsigned it = 0; it < 100000 && idle < 50; it++) {
        unsigned m = move(&C, &S, mangle && (rnd() % 8 == 0)) + move(&S, &C, mangle && (rnd() % 8 == 0));
        if (tls_state(&C) == TLS_FAILED || tls_state(&S) == TLS_FAILED) break;
        if (tls_state(&C) == TLS_CONNECTED && cs < bytes) { uint8_t p[1500]; unsigned k = bytes - cs > sizeof p ? (unsigned)sizeof p : bytes - cs; for (unsigned i = 0; i < k; i++) p[i] = (uint8_t)(cs + i); int w = tls_write(&C, p, k); if (w > 0) cs += (unsigned)w; m += (unsigned)w; }
        if (tls_state(&S) == TLS_CONNECTED && ss < bytes) { uint8_t p[1500]; unsigned k = bytes - ss > sizeof p ? (unsigned)sizeof p : bytes - ss; for (unsigned i = 0; i < k; i++) p[i] = (uint8_t)(0x55 + ss + i); int w = tls_write(&S, p, k); if (w > 0) ss += (unsigned)w; m += (unsigned)w; }
        int n; while ((n = tls_read(&S, d, sizeof d)) > 0) { for (int i = 0; i < n; i++) if (d[i] != (uint8_t)(sr + i)) *delivered_ok = 0; sr += (unsigned)n; m += (unsigned)n; }
        while ((n = tls_read(&C, d, sizeof d)) > 0) { for (int i = 0; i < n; i++) if (d[i] != (uint8_t)(0x55 + cr + i)) *delivered_ok = 0; cr += (unsigned)n; m += (unsigned)n; }
        idle = m ? 0 : idle + 1;
    }
    return cs == bytes && ss == bytes && sr == bytes && cr == bytes && tls_state(&C) == TLS_CONNECTED && tls_state(&S) == TLS_CONNECTED;
}

int main(int argc, char **argv)
{
    if (argc < 4) { fprintf(stderr, "usage: fuzz_tls cert.der priv_hex ca_pub_hex [rounds] [seed]\n"); return 2; }
    FILE *f = fopen(argv[1], "rb"); if (!f) return 2; cert_len = (unsigned)fread(cert, 1, sizeof cert, f); fclose(f);
    if (unhex(argv[2], priv, 32) || unhex(argv[3], ca, 64)) return 2;
    unsigned long rounds = argc > 4 ? strtoul(argv[4], 0, 0) : 2000; x = argc > 5 ? strtoul(argv[5], 0, 0) | 1 : 7;
    int ok;

    /* 1. clean exchange */
    CHECK(exchange(0, 150000, &ok) && ok);
    tls_close(&C); move(&C, &S, 0); { uint8_t d[16]; CHECK(tls_read(&S, d, sizeof d) == TLS_ERR_CLOSED); }                  /* close_notify arrives as a clean close */

    /* 1b. key update both ways, the peer answers a request; traffic continues under the new keys */
    { tls_client_cfg_t cfg = { .ca_pub = ca, .host = "ndf.local" }; tls_server_cfg_t scfg = { cert, cert_len, priv }; uint8_t d[64];
      tls_client_init(&C, &cfg); tls_server_init(&S, &scfg); for (int i = 0; i < 8; i++) { move(&C, &S, 0); move(&S, &C, 0); }
      CHECK(tls_state(&C) == TLS_CONNECTED && tls_state(&S) == TLS_CONNECTED);
      CHECK(tls_key_update(&C, 1) == 0); move(&C, &S, 0); move(&S, &C, 0);
      CHECK(tls_write(&C, "after-c", 7) == 7 && tls_write(&S, "after-s", 7) == 7); move(&C, &S, 0); move(&S, &C, 0);
      CHECK(tls_read(&S, d, sizeof d) == 7 && !memcmp(d, "after-c", 7) && tls_read(&C, d, sizeof d) == 7 && !memcmp(d, "after-s", 7));
      CHECK(tls_key_update(&S, 0) == 0); move(&S, &C, 0); CHECK(tls_write(&S, "again", 5) == 5); move(&S, &C, 0); CHECK(tls_read(&C, d, sizeof d) == 5);
      tls_close(&C); move(&C, &S, 0); CHECK(tls_read(&S, d, sizeof d) == TLS_ERR_CLOSED); CHECK(tls_feed(&S, (const uint8_t *)"\x17\x03\x03\x00\x01x", 6) == 0); }   /* nothing is parsed after close_notify */

    /* 2. wrong trust anchor, wrong name: the client refuses */
    { static tls_client_cfg_t cfg; uint8_t other[64]; memcpy(other, ca, 64); other[5] ^= 1; cfg.ca_pub = other; cfg.host = "ndf.local";
      static tls_server_cfg_t scfg; scfg.cert = cert; scfg.cert_len = cert_len; scfg.priv = priv;
      tls_client_init(&C, &cfg); tls_server_init(&S, &scfg); for (int i = 0; i < 6; i++) { move(&C, &S, 0); move(&S, &C, 0); }
      CHECK(tls_state(&C) == TLS_FAILED && C.err == TLS_ERR_CERT);
      cfg.ca_pub = ca; cfg.host = "other.host"; tls_client_init(&C, &cfg); tls_server_init(&S, &scfg); for (int i = 0; i < 6; i++) { move(&C, &S, 0); move(&S, &C, 0); }
      CHECK(tls_state(&C) == TLS_FAILED && C.err == TLS_ERR_NAME);
      cfg.ca_pub = 0; cfg.pin_leaf = 0; tls_client_init(&C, &cfg); CHECK(tls_state(&C) == TLS_FAILED); }                     /* no trust anchor: never connects */

    /* 3. mangled streams */
    unsigned long done = 0, connected = 0;
    for (unsigned long i = 0; i < rounds && !fails; i++) {
        int ok2; int conn = exchange(1, 20000 + rnd() % 5000, &ok2); done++;
        CHECK(ok2);                                                                                                           /* whatever was delivered is what was sent */
        if (conn) connected++;
    }
    /* 3b. the same over TLS 1.2: a clean exchange (version checked), then mangled streams */
    only_v = 0x0303;
    CHECK(exchange(0, 150000, &ok) && ok && tls_version(&C) == 0x0303 && tls_version(&S) == 0x0303);
    unsigned long done12 = 0, conn12 = 0;
    for (unsigned long i = 0; i < rounds && !fails; i++) { int ok2; int conn = exchange(1, 20000 + rnd() % 5000, &ok2); done12++; CHECK(ok2); if (conn) conn12++; }
    only_v = 0;

    /* 4. X.509: every truncation and random flips of the server certificate */
    x509_t c;
    CHECK(x509_parse(cert, cert_len, &c) == 0 && !x509_verify(&c, ca) && !x509_host_ok(&c, "ndf.local") && !x509_host_ok(&c, "a.example.test") && x509_host_ok(&c, "example.test") && x509_host_ok(&c, "x.y.example.test") && x509_host_ok(&c, "evil.com"));
    for (unsigned n = 0; n < cert_len; n++) { uint8_t *cp = malloc(n ? n : 1); memcpy(cp, cert, n); if (n < cert_len) CHECK(x509_parse(cp, n, &c) != 0); free(cp); }
    for (unsigned long i = 0; i < rounds * 20 && !fails; i++) {
        uint8_t *cp = malloc(cert_len); memcpy(cp, cert, cert_len); unsigned k = 1 + rnd() % 3; while (k--) cp[rnd() % cert_len] = (uint8_t)rnd();
        if (x509_parse(cp, cert_len, &c) == 0) { (void)x509_verify(&c, ca); (void)x509_host_ok(&c, "ndf.local"); }
        free(cp);
    }

    /* 5. garbage into fresh engines */
    for (unsigned long i = 0; i < rounds * 5 && !fails; i++) {
        static tls_client_cfg_t cfg; cfg.ca_pub = ca; cfg.host = "ndf.local"; static tls_server_cfg_t scfg; scfg.cert = cert; scfg.cert_len = cert_len; scfg.priv = priv;
        tls_t *t = (rnd() & 1) ? &C : &S; if (t == &C) tls_client_init(t, &cfg); else tls_server_init(t, &scfg);
        for (int k = 0; k < 4; k++) { uint8_t g[300]; unsigned n = 1 + rnd() % sizeof g; for (unsigned j = 0; j < n; j++) g[j] = (uint8_t)rnd(); if (rnd() & 1) { g[0] = 22; g[1] = 3; g[2] = 3; g[3] = 0; g[4] = (uint8_t)(n - 5 > 255 ? 255 : n > 5 ? n - 5 : 0); }
            unsigned off = 0; while (off < n) { int u = tls_feed(t, g + off, n - off); if (u <= 0) break; off += (unsigned)u; } }
    }
    if (fails) { printf("%d failed\n", fails); return 1; }
    printf("fuzz_tls: ok (%lu mangled exchanges, %lu of them completed; TLS 1.2: %lu, %lu completed)\n", done, connected, done12, conn12);
    return 0;
}
