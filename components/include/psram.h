/* PSRAM: ESP32-S3R8 in-package 8 MB octal; ESP32-P4 16 / 32 MB HEX (drivers/psram/esp32p4). */
#pragma once
#include "ndf.h"
int psram_probe(uint8_t mr[8]);          /* pins + mode registers MR0..MR7 */
int psram_cmd(uint16_t cmd, uint32_t addr, int dummy, const uint8_t *out, int nout, uint8_t *in, int nin);
void psram_pins(void);
void psram_spi0_setup(int clk_div, int rd_dummy, int wr_dummy);
uint32_t psram_bench_jedec(void);
#ifndef PSRAM_VADDR                      /* soc_caps.h sets the P4 window */
#define PSRAM_VADDR     0x3D000000u      /* data-bus window (MMU entries 256..) */
#define PSRAM_MMU_FIRST 256u
#endif
uint32_t psram_init(int clk_div);        /* SRAM clock = 160 MHz / clk_div; returns size in bytes, 0 if absent */
uint32_t psram_size(void);
void psram_writeback(void *p, size_t n); /* cache -> PSRAM */
void psram_invalidate(void *p, size_t n);
#ifdef SOC_GDMA_PSRAM_TX_OK                      /* P4: around light sleep the MPLL and the PSRAM clock are off */
void psram_sleep_enter(void);
void psram_sleep_exit(void);
#endif
