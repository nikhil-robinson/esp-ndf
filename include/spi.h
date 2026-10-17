/* GP-SPI master/slave on SPI2 (host 2) and SPI3 (host 3). Full duplex.
 * Master: <= 64 bytes go through the register buffer (lowest latency),
 * longer transfers use GDMA. Clock from APB 80 MHz: 80/n exactly. */
#pragma once
#include "ndf.h"

/* Returns the achieved SCLK in Hz (exact divider of 80 MHz), 0 on error.
 * Pins < 0 are unused. mode = 0..3 (CPOL/CPHA). */
uint32_t spi_master_init(int host, int sclk, int mosi, int miso, int cs, uint32_t hz, int mode);
/* Full duplex: tx and/or rx may be NULL. Buffers for DMA (> 64 bytes) must be in DRAM. */
int      spi_xfer(int host, const void *tx, void *rx, size_t len);

/* Slave: arm one full-duplex transfer of up to 64 bytes; done when CS rises. */
int      spi_slave_init(int host, int sclk, int mosi, int miso, int cs, int mode);
int      spi_slave_arm(int host, const void *tx, size_t len);
int      spi_slave_result(int host, void *rx, size_t len);   /* bytes received, -1 not done */
