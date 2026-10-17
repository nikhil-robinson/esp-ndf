/* I2S standard (Philips) mode on I2S0/I2S1: stereo, 16/24/32-bit, master or
 * slave, continuous DMA streaming through a ring of blocks (no gaps).
 * MCLK from the 160 MHz PLL via a fractional divider; the achieved sample
 * rate is reported exactly (milli-Hz). */
#pragma once
#include "ndf.h"

typedef struct {
    uint32_t rate;           /* sample rate, Hz */
    uint8_t  bits;           /* 16, 24, 32 */
    bool     slave;          /* clocks (BCK/WS) from outside */
    int mclk, bck, ws, dout, din;   /* -1: unused */
} i2s_cfg_t;

/* Returns the achieved sample rate in milli-Hz (master), cfg rate*1000 for slave, 0 on error. */
uint64_t i2s_init(int port, const i2s_cfg_t *cfg);
void     i2s_start(int port, bool tx, bool rx);
void     i2s_stop(int port);
/* Streaming: copy into / out of the DMA ring. Blocks until space/data or timeout. */
int      i2s_write(int port, const void *buf, size_t len, uint32_t timeout_us);
int      i2s_read(int port, void *buf, size_t len, uint32_t timeout_us);
uint32_t i2s_tx_underruns(int port);   /* blocks played without fresh data (silence) */
uint32_t i2s_rx_overruns(int port);    /* blocks overwritten before being read */
