/* AES accelerator (128/256-bit keys), block mode, plus XTS on top of it. */
#pragma once
#include "ndf.h"

void aes_init(void);
/* engine lock (reentrant on the holding core; interrupts off on it): hold it across several calls that share state */
uint32_t aes_lock(void);
void     aes_unlock(uint32_t st);
/* one 16-byte block; key_len 16 or 32 */
void aes_block(const uint8_t *key, int key_len, bool encrypt, const uint8_t in[16], uint8_t out[16]);
/* XTS-AES-256 (64-byte key: data key | tweak key), n a multiple of 16,
 * tweak = 128-bit little-endian sector number (NVS: entry address). */
void aes_xts(const uint8_t key[64], uint32_t tweak, bool encrypt, const void *in, void *out, size_t n);

/* block modes (NIST SP 800-38A; the engine's DMA mode for runs of NDF_AES_DMA_MIN blocks or more in internal RAM).
 * iv is updated, so consecutive calls chain. ECB/CBC: len a multiple of 16; CTR (32-bit big-endian counter), OFB,
 * CFB128: any length. */
enum { AES_ECB = 0, AES_CBC = 1, AES_OFB = 2, AES_CTR = 3, AES_CFB128 = 5 };
int aes_crypt(int mode, const uint8_t *key, int key_len, bool encrypt, uint8_t iv[16], const void *in, void *out, size_t len);

/* AES-GCM (SP 800-38D), one shot: CTR on the engine + GHASH in software (8-bit table). Any IV length (12 is the fast
 * path). Encrypt writes tag; decrypt checks it (constant time) and returns -1, output zeroed, on mismatch. in may be out. */
int aes_gcm(const uint8_t *key, int key_len, bool encrypt, const uint8_t *iv, size_t iv_len, const void *aad, size_t aad_len,
            const void *in, void *out, size_t len, uint8_t tag[16]);
