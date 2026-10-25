/* X25519 (RFC 7748) Diffie-Hellman in software, constant-time Montgomery ladder. Little-endian 32-byte keys; the
 * scalar is clamped inside. Returns 0, or -1 when the shared secret is all zero (low-order peer key). */
#pragma once
#include <stdint.h>

int x25519(uint8_t out[32], const uint8_t scalar[32], const uint8_t u[32]);
int x25519_base(uint8_t pub[32], const uint8_t priv[32]);         /* pub = priv * 9 */
