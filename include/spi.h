/* GP-SPI master/slave on SPI2 (host 2) and SPI3 (host 3). Full duplex, plus half duplex with
 * command/address/dummy phases on 1/2/4/8 lines (8: SPI2 only) and a half-duplex slave (buffer mode).
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

/* ---- half duplex (flash-style) ---- */
typedef struct {
    uint16_t cmd;  uint8_t cmd_bits;       /* 0..16 bits, MSB first */
    uint32_t addr; uint8_t addr_bits;      /* 0..32 bits, MSB first */
    uint8_t  dummy;                        /* dummy SCLK cycles */
    uint8_t  lines, cmd_lines, addr_lines; /* data / command / address lines: 1 (or 0), 2, 4, 8 */
} spi_hd_t;
/* Data lines beyond MOSI/MISO: line 0 = D(MOSI) 1 = Q(MISO) 2 = WP 3 = HD 4..7 = IO4..7 (SPI2). */
int  spi_master_io(int host, int line, int pin);
void spi_master_cs_timing(int host, uint32_t setup_cycles, uint32_t hold_cycles);
/* One phase sequence: cmd, addr, dummy, then write tx or read rx (not both), len <= 32 KB.
 * spi_xfer() afterwards returns to plain full duplex. */
int  spi_hd_xfer(int host, const spi_hd_t *t, const void *tx, void *rx, size_t len);

/* ---- half-duplex slave, buffer mode (protocol of IDF spi_slave_hd: 8-bit cmd, 8-bit addr, 8 dummy) ---- */
enum { SPI_HD_WRBUF = 0x01, SPI_HD_RDBUF = 0x02, SPI_HD_DUAL = 0x10, SPI_HD_QUAD = 0x20,
       SPI_HD_DUAL_ADDR = 0x50, SPI_HD_QUAD_ADDR = 0xA0 };
int      spi_slave_hd_init(int host, int sclk, int cs, const int8_t io[4], int mode);   /* io: D Q WP HD, -1 unused */
void     spi_slave_hd_buf_write(int host, uint32_t addr, const void *src, size_t len); /* what the master reads */
void     spi_slave_hd_buf_read(int host, uint32_t addr, void *dst, size_t len);        /* what the master wrote */
uint32_t spi_slave_hd_events(int host);    /* bit 11: master wrote the buffer, bit 10: master read it (read clears) */
