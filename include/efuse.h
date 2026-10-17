/* eFuse read access (read-only: burning is deliberately not implemented). */
#pragma once
#include "ndf.h"

/* Read `len` (<= 32) bits starting at bit `pos` of block 1 or 2. */
uint32_t efuse_bits(int block, int pos, int len);
void     efuse_mac(uint8_t mac[6]);
