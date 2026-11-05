/* HMAC-SHA256 with a key held in an eFuse key block (ESP32-P4 only, TRM chapter 29): the key never reaches the CPU. key_id 0..5 is
 * the eFuse block KEY0..KEY5 programmed with the HMAC_UP purpose. Fails (-1) when the block is not set up for it. */
#pragma once
#include "ndf.h"

int hmac_sha256_hw(int key_id, const void *msg, size_t n, uint8_t out[32]);
