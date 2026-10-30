/* Host driver for components/tls: known-answer checks, then the engine over a socket against OpenSSL.
 *   tls_host selftest
 *   tls_host client <host> <port> <ca_pub_hex128|-> <sni|-> <path> [<pin_leaf_hex128>]   GET <path>, print the body length and a checksum
 *   tls_host server <port> <cert.der> <priv_hex64> [bytes]                                         accept one client, read until close_notify, answer "got N bytes sum S"
 * The board's SHA and AES engines are replaced by a small software SHA-256 and OpenSSL's AES-GCM. */
#include "tls.h"
#include "aes.h"
#include "rng.h"
#include <arpa/inet.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "tls_shims.c"

static int hex(const char *s, uint8_t *o, unsigned n) { if (strlen(s) != 2 * n) return -1; for (unsigned i = 0; i < n; i++) { unsigned v; sscanf(s + 2 * i, "%2x", &v); o[i] = (uint8_t)v; } return 0; }

static int kat(const char *what, const uint8_t *got, const char *want)
{
    uint8_t w[64]; unsigned n = (unsigned)strlen(want) / 2; hex(want, w, n); int ok = !memcmp(got, w, n); printf("%s %s\n", ok ? "ok  " : "FAIL", what); return ok;
}
static int selftest(void)
{
    int ok = 1; uint8_t o[32], key[32]; memset(key, 0x0b, 20);
    tls_hmac(key, 20, (const uint8_t *)"Hi There", 8, o); ok &= kat("HMAC-SHA256 RFC 4231 case 1", o, "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");
    tls_hmac((const uint8_t *)"Jefe", 4, (const uint8_t *)"what do ya want for nothing?", 28, o); ok &= kat("HMAC-SHA256 RFC 4231 case 2", o, "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
    uint8_t zero[32] = { 0 }, early[32], empty[32], derived[32];
    tls_hkdf_extract(0, 0, zero, 32, early); ok &= kat("TLS 1.3 early secret (RFC 8448)", early, "33ad0a1c607ec03b09e6cd9893680ce210adf300aa1f2660e1b22e10f170f92a");
    sha(SHA256, "", 0, empty); tls_derive_secret(early, "derived", empty, derived); ok &= kat("TLS 1.3 derived secret (RFC 8448)", derived, "6f2615a108c702c5678f54fc9dbab69716c076189c48250cebeac3576c3611ba");
    uint8_t ikm[22], salt[13], info[10], okm[32], prk[32]; memset(ikm, 0x0b, 22); for (int i = 0; i < 13; i++) salt[i] = (uint8_t)i; for (int i = 0; i < 10; i++) info[i] = (uint8_t)(0xf0 + i);
    tls_hkdf_extract(salt, 13, ikm, 22, prk); ok &= kat("HKDF-Extract RFC 5869 case 1", prk, "077709362c2e32df0ddc3f0dc47bba6390b6c73bb50f9c3122ec844ad7c2b3e5");
    uint8_t m[11]; memcpy(m, info, 10); m[10] = 1; tls_hmac(prk, 32, m, 11, okm); ok &= kat("HKDF-Expand block 1 RFC 5869 case 1", okm, "3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf");
    return ok ? 0 : 1;
}

static int dial(const char *host, int port)
{
    struct addrinfo hints = { .ai_socktype = SOCK_STREAM }, *r; char ps[8]; snprintf(ps, sizeof ps, "%d", port);
    if (getaddrinfo(host, ps, &hints, &r)) return -1;
    int s = socket(r->ai_family, SOCK_STREAM, 0); if (connect(s, r->ai_addr, r->ai_addrlen)) { close(s); s = -1; }
    freeaddrinfo(r); return s;
}
/* send what the engine queued; with wait_read also feed received bytes (what a previous call could not hand over first, else one read). < 0: socket ended or engine error */
static uint8_t rb[5000]; static ssize_t rlen, roff;
static int pump(tls_t *t, int s, int wait_read)
{
    unsigned n; const uint8_t *o = tls_out(t, &n);
    while (n) { ssize_t w = write(s, o, n); if (w <= 0) return -100; tls_out_done(t, (unsigned)w); o = tls_out(t, &n); }
    if (!wait_read) return 0;
    if (roff >= rlen) { rlen = read(s, rb, sizeof rb); roff = 0; if (rlen <= 0) { rlen = 0; return -101; } }
    int u = tls_feed(t, rb + roff, (unsigned)(rlen - roff)); if (u < 0) return u;
    roff += u;
    return 0;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, 0, _IONBF, 0);
    if (argc > 1 && !strcmp(argv[1], "selftest")) return selftest();
    static tls_t t;
    if (argc >= 7 && !strcmp(argv[1], "client")) {
        uint8_t ca[64], pin[64]; tls_client_cfg_t cfg = { 0 };
        cfg.host = strcmp(argv[5], "-") ? argv[5] : 0;
        if (strcmp(argv[4], "-")) { if (hex(argv[4], ca, 64)) return 2; cfg.ca_pub = ca; }
        if (argc > 7) { if (hex(argv[7], pin, 64)) return 2; cfg.pin_leaf = pin; }
        int s = dial(argv[2], atoi(argv[3])); if (s < 0) { printf("connect failed\n"); return 3; }
        tls_client_init(&t, &cfg);
        int r = 0; while (r >= 0 && tls_state(&t) == TLS_HANDSHAKE) r = pump(&t, s, 1);
        if (tls_state(&t) != TLS_CONNECTED) { pump(&t, s, 0); printf("handshake failed %d\n", t.err ? t.err : r); return 4; }
        printf("version %04x\n", tls_version(&t));
        char req[300]; int rl = snprintf(req, sizeof req, "GET %s HTTP/1.0\r\n\r\n", argv[6]);
        for (int off = 0; off < rl; ) { int w = tls_write(&t, req + off, (unsigned)(rl - off)); off += w; pump(&t, s, 0); }
        unsigned long sum = 0, body = 0; static uint8_t d[5000]; int hdr_done = 0; char head[600]; unsigned hl = 0;
        for (;;) {
            int n = tls_read(&t, d, sizeof d);
            if (n > 0) { for (int i = 0; i < n; i++) { if (!hdr_done) { if (hl < sizeof head - 1) head[hl++] = (char)d[i]; if (hl >= 4 && !memcmp(head + hl - 4, "\r\n\r\n", 4)) hdr_done = 1; } else { body++; sum += d[i]; } } continue; }
            if (n == TLS_ERR_CLOSED) break;
            if (n < 0) { printf("read error %d\n", n); return 5; }
            if (pump(&t, s, 1) < 0) break;
        }
        head[hl] = 0; printf("status %.12s body %lu sum %lu\n", head, body, sum);
        tls_close(&t); pump(&t, s, 0); close(s); return 0;
    }
    if (argc >= 5 && !strcmp(argv[1], "server")) {
        static uint8_t cert[3000], priv[32]; FILE *f = fopen(argv[3], "rb"); if (!f) return 2; unsigned cl = (unsigned)fread(cert, 1, sizeof cert, f); fclose(f);
        if (hex(argv[4], priv, 32)) return 2;
        tls_server_cfg_t cfg = { cert, cl, priv };
        int ls = socket(AF_INET, SOCK_STREAM, 0), one = 1; setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons((uint16_t)atoi(argv[2])), .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
        if (bind(ls, (struct sockaddr *)&a, sizeof a) || listen(ls, 1)) return 3;
        printf("listening\n"); fflush(stdout);
        int s = accept(ls, 0, 0);
        tls_server_init(&t, &cfg);
        int r = 0; while (r >= 0 && tls_state(&t) == TLS_HANDSHAKE) r = pump(&t, s, 1);
        if (tls_state(&t) != TLS_CONNECTED) { pump(&t, s, 0); printf("handshake failed %d\n", t.err ? t.err : r); return 4; }
        printf("version %04x\n", tls_version(&t));
        unsigned long total = 0, sum = 0, want = argc > 5 ? strtoul(argv[5], 0, 0) : 0; static uint8_t d[5000];     /* want: answer after this many bytes (openssl s_client does not close first) */
        for (;;) {
            int n = tls_read(&t, d, sizeof d);
            if (n > 0) { total += (unsigned long)n; for (int i = 0; i < n; i++) sum += d[i]; if (want && total >= want) break; continue; }
            if (n == TLS_ERR_CLOSED) break;
            if (n < 0) { printf("read error %d\n", n); return 5; }
            if (pump(&t, s, 1) < 0) break;
        }
        if (tls_state(&t) == TLS_FAILED) { pump(&t, s, 0); printf("read error %d\n", t.err); return 5; }        /* the alert has gone out */
        char rep[80]; int rl = snprintf(rep, sizeof rep, "got %lu bytes sum %lu\n", total, sum);
        for (int off = 0; off < rl; ) { int w = tls_write(&t, rep + off, (unsigned)(rl - off)); if (w <= 0 && tls_state(&t) != TLS_CONNECTED) break; off += w; pump(&t, s, 0); }
        printf("%s", rep); tls_close(&t); pump(&t, s, 0); close(s); return 0;
    }
    fprintf(stderr, "usage: see the head of tools/tls_host.c\n"); return 1;
}
