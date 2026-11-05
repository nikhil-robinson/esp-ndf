/* Digital Signature peripheral (ESP32-P4 only, TRM chapter 32): RSA signature (PKCS#1 v1.5 padding done by the peripheral) with
 * private-key parameters that are stored encrypted (AES-GCM-like, key derived by the HMAC block from an eFuse key block with the
 * DS purpose). The CPU only ever sees the encrypted parameters `c` and the message. The parameter blob and its IV are produced at
 * provisioning time (as with esp-idf's `esp_ds_data_t`, the same format); this driver never programs eFuses. */
#pragma once
#include "ndf.h"

#define DS_MAX_BITS  4096
#define DS_C_LEN     ((DS_MAX_BITS * 3 + 256 + 32 + 32 + 64) / 8)      /* Y, M, r_bar at the maximum length, MD, M', L, padding */
typedef struct {
    uint8_t  iv[16];
    uint8_t  c[DS_C_LEN];
    uint32_t rsa_length;          /* key length in 32-bit words minus 1: 1024 -> 31, 2048 -> 63, 3072 -> 95, 4096 -> 127 */
} ds_data_t;

/* signature = message ^ d mod n; message and signature are rsa_length + 1 words, little-endian. hmac_key: eFuse block 0..5.
 * Returns 0, -1 invalid key / HMAC setup, -2 wrong digest (parameters corrupted), -3 wrong padding. */
int ds_sign(const ds_data_t *d, int hmac_key, const uint8_t *message, uint8_t *signature);
