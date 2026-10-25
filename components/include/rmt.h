/* Remote control transceiver: 4 TX (0..3) + 4 RX (0..3) channels, pulse
 * trains of {level, duration} items. Any length: TX refills and RX drains
 * the 48-word channel RAM half by half from the interrupt. */
#pragma once
#include "ndf.h"
#include "spin.h"

/* one item = two pulses: duration in ticks (1..32767), level 0/1 */
typedef union {
    struct { uint32_t d0 : 15, l0 : 1, d1 : 15, l1 : 1; };
    uint32_t val;
} rmt_item_t;
#define RMT_ITEM(lv0, dur0, lv1, dur1) \
    ((rmt_item_t){ .d0 = (dur0), .l0 = (lv0), .d1 = (dur1), .l1 = (lv1) })

/* resolution = 80 MHz / n (n = 1..255). Returns actual Hz, 0 on error. */
uint32_t rmt_tx_init(int ch, int pin, uint32_t resolution_hz, bool idle_level);
/* Start sending n items (no terminator needed). Buffer must stay valid
 * until rmt_tx_wait() returns. */
int      rmt_tx(int ch, const rmt_item_t *items, size_t n);
bool     rmt_tx_wait(int ch);                 /* false: still sending after 60 s (loop mode: rmt_tx_stop) */
/* Carrier modulation of high pulses (IR): hz, duty in %. hz = 0 off. */
void     rmt_tx_carrier(int ch, uint32_t hz, int duty_pct);

/* RX: pulses shorter than filter_ns are dropped; a level held longer than
 * idle_ticks ends the frame. */
uint32_t rmt_rx_init(int ch, int pin, uint32_t resolution_hz, uint32_t idle_ticks, uint32_t filter_ns);
int      rmt_rx_start(int ch, rmt_item_t *buf, size_t max);
/* Wait for the frame end (or timeout_us). Returns items received, -1 timeout. */
int      rmt_rx_wait(int ch, uint32_t timeout_us);

/* Loop: send n (<= 47) items `loops` times (1..1023) back to back, 0 = until rmt_tx_stop(). rmt_tx_wait ends. */
int      rmt_tx_loop(int ch, const rmt_item_t *items, size_t n, uint32_t loops);
void     rmt_tx_stop(int ch);
/* Start TX channels in `mask` on the same tick: set, then rmt_tx() each; 0 = off. */
void     rmt_tx_sync(uint32_t mask);
/* RX carrier demodulation: pulses of a carrier (hz, duty) inside a level merge into one level. hz = 0 off. */
void     rmt_rx_demod(int ch, uint32_t hz, uint32_t resolution_hz, int duty_pct, bool active_high);
/* DMA (TX ch 3, RX ch 3 only): no per-half interrupts. Buffers in internal RAM, <= 8 x 1023 items. */
int      rmt_tx_dma(int ch, const rmt_item_t *items, size_t n);
int      rmt_rx_dma_start(int ch, rmt_item_t *buf, size_t max);
int      rmt_rx_dma_wait(int ch, uint32_t timeout_us);
/* WS2812B encoder: 8 items per byte (GRB order, MSB first) at resolution res_hz (10 MHz suits). */
size_t   rmt_ws2812_encode(const uint8_t *grb, size_t nbytes, rmt_item_t *out, uint32_t res_hz);
