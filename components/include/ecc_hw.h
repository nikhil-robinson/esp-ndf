/* ECC accelerator (ESP32-P4 only, TRM chapter 28): point multiplication, point verification / add and modular arithmetic on
 * the NIST curves P-192, P-256 and P-384 in hardware. Operands are little-endian byte arrays of the curve size (24 / 32 / 48
 * bytes). Names follow drivers/crypto/ecc.c (software) where the operation is the same. Blocking, one user at a time. */
#pragma once
#include "ndf.h"

typedef enum { ECC_P192 = 0, ECC_P256 = 1, ECC_P384 = 2 } ecc_curve_id_t;
typedef enum { ECC_MOD_ADD, ECC_MOD_SUB, ECC_MOD_MUL, ECC_MOD_INV } ecc_mod_op_t;

int ecc_hw_mul(ecc_curve_id_t c, const uint8_t *k, const uint8_t *px, const uint8_t *py, uint8_t *rx, uint8_t *ry);       /* R = k P; 0, or -1 (point not on the curve) */
int ecc_hw_verify(ecc_curve_id_t c, const uint8_t *px, const uint8_t *py);                                            /* 1: P is on the curve, 0: not */
int ecc_hw_add(ecc_curve_id_t c, const uint8_t *px, const uint8_t *py, const uint8_t *qx, const uint8_t *qy, uint8_t *rx, uint8_t *ry);   /* R = P + Q (affine) */
int ecc_hw_mod(ecc_curve_id_t c, ecc_mod_op_t op, bool mod_n, const uint8_t *a, const uint8_t *b, uint8_t *r);        /* r = a op b mod p (mod_n: mod the group order n) */
