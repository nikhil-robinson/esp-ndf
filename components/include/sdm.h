/* Sigma-delta modulator: 8 channels, 1-bit output whose average duty is
 * (density + 128) / 256. Low-pass filter it for a cheap DAC. */
#pragma once
#include "ndf.h"

/* clk_hz = 80 MHz / n, n = 1..256 (exact divider or refused, returns 0) */
uint32_t sdm_init(int ch, int pin, uint32_t clk_hz);
void     sdm_density(int ch, int8_t density);      /* -128 .. 127 */
