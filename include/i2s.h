/* I2S on I2S0/I2S1, continuous DMA streaming through a ring of blocks (no gaps):
 *  - STD: stereo, 8/16/24/32-bit, master or slave; TX+RX on one port = full duplex
 *  - TDM: 2-16 slots per frame (frame <= 128 bits), slot mask
 *  - framing (STD and TDM, `fmt`): Philips (WS half a frame, data 1 bit late), MSB-aligned (no delay),
 *         PCM short (1-bit WS pulse before the frame), PCM long (WS one slot wide); same as IDF's slot presets
 *  - PDM (I2S0 only): TX PCM->PDM with IDF's default filter set, clock = rate*128; RX PDM->PCM,
 *         decimation 64 (clock rate*64), or 128 in duplex (shares TX's clock); 16-bit stereo. Clock on `ws`.
 *  Master with both dout and din = full duplex: RX runs as slave on TX's BCK/WS inside the port.
 * MCLK from the 160 MHz PLL via a fractional divider; the achieved sample
 * rate is reported exactly (milli-Hz). */
#pragma once
#include "ndf.h"

enum { I2S_STD = 0, I2S_TDM, I2S_PDM };
enum { I2S_PHILIPS = 0, I2S_MSB, I2S_PCM_SHORT, I2S_PCM_LONG };

typedef struct {
    uint8_t  mode;           /* I2S_STD (default), I2S_TDM, I2S_PDM */
    uint8_t  fmt;            /* STD/TDM framing: I2S_PHILIPS (default), I2S_MSB, I2S_PCM_SHORT, I2S_PCM_LONG */
    uint8_t  slots;          /* TDM: total slots per frame (0 = 2) */
    uint16_t mask;           /* TDM: active slots (0 = all). Buffers hold active slots only; inactive TX slots send 0 */
    uint32_t rate;           /* sample rate, Hz */
    uint8_t  bits;           /* 8, 16, 24, 32 (PDM: 16) */
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
