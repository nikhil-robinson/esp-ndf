/* NIST P-256 (secp256r1) in software: key pairs, ECDH, ECDSA. The S3 has no ECC block; IDF uses mbedtls' generic
 * bignum code. Keys and numbers are big-endian bytes as in SEC 1: private key 32 bytes, public key X||Y 64 bytes,
 * signature r||s 64 bytes, hash 32 bytes (SHA-256). Scalar multiplication is constant-time (fixed window, masked
 * table lookup). All functions return 0 on success. */
#pragma once
#include <stdint.h>

int p256_keygen(uint8_t priv[32], uint8_t pub[64]);                       /* random private key (rng_fill) */
int p256_pubkey(uint8_t pub[64], const uint8_t priv[32]);                 /* priv in [1, n-1] */
int p256_check_pub(const uint8_t pub[64]);                                /* on the curve */
int p256_ecdh(uint8_t shared[32], const uint8_t priv[32], const uint8_t peer[64]);   /* X of priv*peer */
int p256_sign(uint8_t sig[64], const uint8_t priv[32], const uint8_t hash[32]);      /* random k */
int p256_sign_k(uint8_t sig[64], const uint8_t priv[32], const uint8_t hash[32], const uint8_t k[32]);
int p256_verify(const uint8_t pub[64], const uint8_t hash[32], const uint8_t sig[64]);  /* 0 = valid */
