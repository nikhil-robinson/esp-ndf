/* Minimal X.509 (RFC 5280) for TLS: ECDSA P-256 / SHA-256 certificates only. Pure, every length checked against the buffer; the host tests and the fuzzer drive it. */
#pragma once
#include <stdint.h>

typedef struct {
    const uint8_t *tbs; unsigned tbs_len;          /* the signed part (tbsCertificate, with its header) */
    uint8_t pub[64];                               /* subject public key X||Y */
    uint8_t sig[64];                               /* signature r||s */
    const uint8_t *san; unsigned san_len;          /* GeneralNames inside the subjectAltName extension (0 = none) */
    const uint8_t *cn; unsigned cn_len;            /* subject common name (0 = none) */
    uint8_t is_ca;
} x509_t;

int x509_parse(const uint8_t *der, unsigned n, x509_t *c);                     /* 0 ok, -1 malformed or not an ECDSA P-256 / SHA-256 certificate */
int x509_verify(const x509_t *c, const uint8_t ca_pub[64]);                    /* 0 = signed by that key */
int x509_host_ok(const x509_t *c, const char *host);                           /* SAN dNSName, else CN; one leading "*." wildcard label; 0 = matches */
int x509_ecdsa_der(const uint8_t *der, unsigned n, uint8_t rs[64]);            /* DER SEQUENCE { INTEGER r, INTEGER s } -> r||s; 0 ok */
unsigned x509_ecdsa_to_der(const uint8_t rs[64], uint8_t *out);                /* r||s -> DER, returns the length (<= 72) */
