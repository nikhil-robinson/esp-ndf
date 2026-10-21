/* Hardware packet engine (rf.h, "packets"). The ESP32-S3's link-layer core is used only as a framer / deframer:
 * send = its connection-master format with no connection (one packet per event, re-kicked from the end interrupt);
 * receive = its connection-slave format (takes up to 255 bytes) with the BT TX state PA enable cleared, so the reply
 * the core would send is never radiated. How every setting was found: docs/RF_API.md, bench/rfmeas/raw_link.txt. */
#include "rf.h"
#include "ble_hw.h"
#include "rf_init.h"

static int ready;
static int8_t chan_of(uint16_t mhz)                 /* core channel index: 37 = 2402, 0..10 = 2404..2424, 38 = 2426, 11..36 = 2428..2478, 39 = 2480 */
{
    if (mhz < 2402 || mhz > 2480 || (mhz & 1)) return -1;
    if (mhz == 2402) return 37;
    if (mhz == 2426) return 38;
    if (mhz == 2480) return 39;
    return (int8_t)(mhz < 2426 ? (mhz - 2404) / 2 : (mhz - 2406) / 2);
}
static int setup(const rf_pkt_cfg_t *c, int8_t *ch)
{
    if (rf_state() == RF_STATE_OFF) return RF_ERR_STATE;
    if (!c || c->rate > RF_PKT_500K || (*ch = chan_of(c->mhz)) < 0) return RF_ERR_RANGE;
    if (c->flags & RF_PKT_WHITEN_OFF) return RF_ERR_RANGE;   /* no clean whitening switch found yet (docs/RF_API.md) */
    if (!ready) { if (ble_hw_init()) return RF_ERR_STATE; ready = 1; }
    rf_pkt_stop();
    ble_hw_raw_flags = c->flags & RF_PKT_CRC_OFF;
    return RF_OK;
}

int rf_pkt_send(const rf_pkt_cfg_t *c, uint8_t hdr, const uint8_t *data, unsigned len, uint32_t count)
{
    int8_t ch; int rc = setup(c, &ch);
    if (rc) return rc;
    if (len > 255) return RF_ERR_RANGE;
    rf_limits_t l; rf_get_limits(&l);
    if (c->mhz * 1000u < l.tx_min_khz || c->mhz * 1000u > l.tx_max_khz) return RF_ERR_DENIED;
    return ble_hw_raw_sendn((uint8_t)ch, c->rate, c->sync, c->crc_init, hdr, data, len, count) ? RF_ERR_RANGE : RF_OK;
}

static int rx_forced;
int rf_pkt_bench_forced;                         /* bench: forced front end for every rate */
int rf_pkt_rx_start(const rf_pkt_cfg_t *c, int gain)
{
    int8_t ch; int rc = setup(c, &ch);
    if (rc) return rc;
    if (gain > 127) return RF_ERR_RANGE;
    if (ble_hw_raw_rx((uint8_t)ch, c->rate, c->sync, c->crc_init)) return RF_ERR_RANGE;
    if (c->rate >= RF_PKT_125K || rf_pkt_bench_forced) {                /* coded: hardware-sequenced receive is erratic (0..81% per run, measured);
                                                  * the front end forced to receive (TX path off, gain code 127) takes every packet */
        rf_pbus_force_mode(1); rf_pbus_xpd_tx_off(); rf_pbus_xpd_rx_on(127);
        rx_forced = 2;
        return RF_OK;
    }
    rf_rx_gain(gain);                            /* < 0: hardware AGC; else the AGC held at this gain */
    rf_pbus_mem_write(42, rf_pbus_mem_default(42) & ~0xF0u);   /* receive only: the BT TX state PA enable (pbus memory word 42, bits 7:4) cleared; RX states and AGC untouched */
    rx_forced = 1;
    return RF_OK;
}

int rf_pkt_read(rf_pkt_t *p)
{
    unsigned len;
    if (!ble_hw_raw_read2(p->data, sizeof p->data, &len, &p->status, &p->rssi, &p->hdr)) return 0;
    p->len = (uint8_t)len; p->ok = !(p->status & 0x0F);
    return 1;
}

int rf_pkt_busy(void) { return ble_hw_raw_busy(); }

void rf_pkt_stop(void)
{
    if (!ready) return;
    ble_hw_raw_stop();
    if (rx_forced == 1) { rf_pbus_mem_write(42, rf_pbus_mem_default(42)); rf_rx_gain(-1); }
    if (rx_forced == 2) rf_pbus_force_mode(0);
    rx_forced = 0;
}

int rf_pkt_sync_ok(uint32_t s)
{
    int run = 1, maxrun = 1, tr = 0;
    for (int k = 1; k < 32; k++) {
        if (((s >> k) & 1) == ((s >> (k - 1)) & 1)) { if (++run > maxrun) maxrun = run; }
        else { run = 1; tr++; }
    }
    int top = 0;
    for (int k = 27; k < 32; k++) top += ((s >> k) & 1) != ((s >> (k - 1)) & 1);
    int same = (s & 0xFF) == ((s >> 8) & 0xFF) && (s & 0xFF) == ((s >> 16) & 0xFF) && (s & 0xFF) == (s >> 24);
    return maxrun <= 6 && tr <= 24 && top >= 2 && !same;
}
