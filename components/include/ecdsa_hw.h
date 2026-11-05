/* ECDSA accelerator (ESP32-P4 only, TRM chapter 33) on P-192 / P-256 / P-384: signature verification with a public key given
 * in software, signing and public-key export with a private key held in an eFuse key block (purpose ECDSA_KEY; the key never
 * reaches the CPU). The driver never programs eFuses: provisioning is done outside. Operands are little-endian byte arrays of
 * the curve size (24 / 32 / 48 bytes); the hash is the digest, already truncated to that size. */
#pragma once
#include "ndf.h"
#include "ecc_hw.h"

int ecdsa_hw_verify(ecc_curve_id_t c, const uint8_t *hash, const uint8_t *r, const uint8_t *s, const uint8_t *qx, const uint8_t *qy);   /* 0: valid, -1: invalid / error */
int ecdsa_hw_sign(ecc_curve_id_t c, int efuse_block, const uint8_t *hash, uint8_t *r, uint8_t *s);        /* efuse_block: KEY0..KEY5 = 0..5 (P-384: low | high << 4) */
int ecdsa_hw_pubkey(ecc_curve_id_t c, int efuse_block, uint8_t *qx, uint8_t *qy);
