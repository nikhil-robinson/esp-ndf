/* AES accelerator (128/256-bit keys), block mode, plus XTS on top of it. */
#pragma once
#include "ndf.h"

void aes_init(void);
/* one 16-byte block; key_len 16 or 32 */
void aes_block(const uint8_t *key, int key_len, bool encrypt, const uint8_t in[16], uint8_t out[16]);
/* XTS-AES-256 (64-byte key: data key | tweak key), n a multiple of 16,
 * tweak = 128-bit little-endian sector number (NVS: entry address). */
void aes_xts(const uint8_t key[64], uint32_t tweak, bool encrypt, const void *in, void *out, size_t n);
