/* Baseband dump engine: raw ADC IQ into internal SRAM (register map from
 * esp_sdr, verified on hardware). A word holds I in bits 9:0 and Q in bits
 * 19:10 (signed 10-bit). */
#pragma once
#include <stdint.h>

#define RF_DUMP_BURST_ADDR 0x3FCD0000u          /* bank 2; the caller keeps 0x3FCB0000..0x3FCE0000 out of the heap */
#define RF_DUMP_MAX        16383u

enum { RF_DUMP_80M = 0, RF_DUMP_40M = 1u << 15, RF_DUMP_16M = 1u << 16 };

static inline int rf_iq_i(uint32_t w) { return (int32_t)(w << 22) >> 22; }
static inline int rf_iq_q(uint32_t w) { return (int32_t)(w << 12) >> 22; }

/* One-shot capture of n words at RF_DUMP_BURST_ADDR; 0 on success, -1 on timeout. */
int rf_dump_burst(uint32_t rate, uint32_t n);
