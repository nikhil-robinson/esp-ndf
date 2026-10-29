/* TLS 1.3 (RFC 8446), client and server, our own code. Sans-IO: the caller feeds the bytes it received and sends the bytes the engine has queued, so the
 * same engine runs over an lwIP pcb on the board and over a socket in the host tests. One cipher suite (TLS_AES_128_GCM_SHA256), X25519, ECDSA P-256 /
 * SHA-256, no PSK / resumption / 0-RTT / client certificates / HelloRetryRequest. Records are limited to 4096 bytes of plaintext (max_fragment_length).
 * A handshake message must fit one record. One connection per tls_t; a tls_t is about 10 KB. See docs/superpowers/plans/2026-10-29-tls.md. */
#pragma once
#include <stdint.h>
#include "x509.h"
#include "sha.h"

#ifndef TLS_REC_PLAIN
#define TLS_REC_PLAIN 4096          /* largest record plaintext we accept and send; a peer that sends more gets record_overflow (RAM: 2 x (this + 261) per connection) */
#endif
#define TLS_REC_BUF   (5 + TLS_REC_PLAIN + 256)
#ifndef TLS_WBUF
#define TLS_WBUF      2560          /* output queue: a server flight (certificate up to about 1.7 KB) or one application record; tls_write takes what fits */
#endif

enum { TLS_HANDSHAKE = 0, TLS_CONNECTED, TLS_CLOSED /* peer sent close_notify */, TLS_FAILED };
enum { TLS_OK = 0, TLS_ERR_PROTO = -1, TLS_ERR_UNSUPPORTED = -2, TLS_ERR_CERT = -3, TLS_ERR_NAME = -4, TLS_ERR_SIG = -5, TLS_ERR_FINISHED = -6,
       TLS_ERR_MAC = -7, TLS_ERR_OVERFLOW = -8, TLS_ERR_ALERT = -9 /* the peer sent a fatal alert */, TLS_ERR_CLOSED = -10 };

typedef struct {
    const uint8_t *ca_pub;      /* 64-byte P-256 point (X||Y): the server certificate must be signed by it */
    const uint8_t *pin_leaf;    /* or: accept exactly this leaf public key (self-signed servers); at least one of the two is required */
    const char *host;           /* sent as SNI and checked against the certificate (SAN dNSName, else CN); NULL = neither */
} tls_client_cfg_t;
typedef struct { const uint8_t *cert; unsigned cert_len; const uint8_t *priv; /* 32-byte P-256 private key of the certificate */ } tls_server_cfg_t;

typedef struct tls {
    uint8_t is_server, state, hs, err_alert; int8_t err;
    uint8_t rbuf[TLS_REC_BUF]; uint16_t rfill, rplain_off, rplain_len;         /* received record being assembled; decrypted application data waits in it */
    uint8_t wbuf[TLS_WBUF]; uint16_t wlen;                                   /* bytes queued for the peer */
    uint8_t wkey[16], wiv[12], rkey[16], riv[12]; uint64_t wseq, rseq; uint8_t wenc, renc;
    uint8_t sec_hs_c[32], sec_hs_s[32], sec_ap_c[32], sec_ap_s[32], master[32];  /* traffic secrets; master only until the application keys exist */
    uint8_t priv[32]; uint8_t sess[32], sess_len; sha_ctx_t th;                 /* ephemeral X25519 key, legacy session id (echoed by the server), transcript hash */
    const tls_client_cfg_t *ccfg; const tls_server_cfg_t *scfg;
    uint8_t fin_hash[32]; uint8_t leaf[64]; uint8_t mfl;
} tls_t;

void tls_client_init(tls_t *t, const tls_client_cfg_t *c);                    /* queues the ClientHello */
void tls_server_init(tls_t *t, const tls_server_cfg_t *c);
int  tls_feed(tls_t *t, const uint8_t *in, unsigned n);                        /* bytes consumed (may be less than n while application data waits to be read), < 0 error */
const uint8_t *tls_out(tls_t *t, unsigned *n);                                 /* bytes to send */
void tls_out_done(tls_t *t, unsigned n);                                       /* n of them were sent */
int  tls_write(tls_t *t, const void *p, unsigned n);                           /* bytes accepted (0 = output queue full, or handshake not done) */
int  tls_read(tls_t *t, void *p, unsigned max);                                /* application bytes; 0 = none now; TLS_ERR_CLOSED after close_notify, < 0 failed */
int  tls_state(const tls_t *t);
void tls_close(tls_t *t);                                                      /* queue close_notify */

/* test hooks: fixed randomness for the known-answer tests */
void tls_test_fix_random(const uint8_t rnd[32], const uint8_t x25519_priv[32], const uint8_t sess[32]);

/* crypto helpers (tls_crypto.c); exposed for the tests */
void tls_hmac(const uint8_t *key, unsigned kl, const uint8_t *m, unsigned ml, uint8_t out[32]);
void tls_hkdf_extract(const uint8_t *salt, unsigned sl, const uint8_t *ikm, unsigned il, uint8_t out[32]);
void tls_expand_label(const uint8_t secret[32], const char *label, const uint8_t *ctx, unsigned cl, uint8_t *out, unsigned len);   /* HKDF-Expand-Label, len <= 32 */
void tls_derive_secret(const uint8_t secret[32], const char *label, const uint8_t hash[32], uint8_t out[32]);
