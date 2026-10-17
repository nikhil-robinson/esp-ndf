/* Remote control transceiver: 4 TX (0..3) + 4 RX (0..3) channels, pulse
 * trains of {level, duration} items. Any length: TX refills and RX drains
 * the 48-word channel RAM half by half from the interrupt. */
#pragma once
#include "ndf.h"

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
void     rmt_tx_wait(int ch);
/* Carrier modulation of high pulses (IR): hz, duty in %. hz = 0 off. */
void     rmt_tx_carrier(int ch, uint32_t hz, int duty_pct);

/* RX: pulses shorter than filter_ns are dropped; a level held longer than
 * idle_ticks ends the frame. */
uint32_t rmt_rx_init(int ch, int pin, uint32_t resolution_hz, uint32_t idle_ticks, uint32_t filter_ns);
int      rmt_rx_start(int ch, rmt_item_t *buf, size_t max);
/* Wait for the frame end (or timeout_us). Returns items received, -1 timeout. */
int      rmt_rx_wait(int ch, uint32_t timeout_us);
