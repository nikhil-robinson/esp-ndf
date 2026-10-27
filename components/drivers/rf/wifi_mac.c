/* Wi-Fi MAC driver v2: FCS in both directions, TX/RX switching, counters (see wifi_mac.h). */
#include <string.h>
#include "wifi_mac.h"
#include "rf.h"
#include "crc32.h"
#include "sys.h"
#include "xtensa.h"

#define MAXF 1600
#ifndef WIFI_QUIET_US
#define WIFI_QUIET_US 4000
#endif
#define R32(a) (*(volatile uint32_t *)(a))

static uint8_t my[6], chan = 6, buf[MAXF];
uint32_t n_drop_mine, n_mine, n_retry; static uint64_t last_mine;
static uint32_t n_rx, n_drop, n_tx, n_fail;

static int listen(void)                                      /* RX front end + ring + address filter (the register replay overwrites it) */
{
    int rc = rf_wifi_rx_init(); if (rc) return rc;
    R32(0x60033040) = my[0] | my[1] << 8 | my[2] << 16 | (uint32_t)my[3] << 24; R32(0x60033044) = my[4] | my[5] << 8;
    R32(0x60033060) = 0xFFFFFFFFu; R32(0x60033064) = 0x0001FFFFu;                 /* match all 48 bits of slot 0, filter enabled (bit 16) */
    return RF_OK;
}

int wifi_mac_init(const uint8_t mac[6])
{
    memcpy(my, mac, 6);
    int rc = rf_wifi_set_channel(chan); if (rc) return rc;
    return listen();
}

int wifi_mac_set_channel(uint8_t ch)
{
    int rc = rf_wifi_set_channel(ch); if (rc) return rc;
    chan = ch;
    return listen();
}

/* Mbit/s of an IDF rate code (wifi_phy_rate_t), enough for the airtime wait; unknown codes count as 1 */
static unsigned mbps(unsigned r) { switch (r) { case 1: return 2; case 3: return 11; case 0xB: return 6; case 0xF: return 9; case 0xA: return 12; case 0xE: return 18; case 9: return 24; case 0xD: return 36; default: return 1; } }

int wifi_mac_tx(const uint8_t *frame, unsigned len) { return wifi_mac_tx_rate(frame, len, WIFI_RATE_1M); }

int wifi_mac_tx_rate(const uint8_t *frame, unsigned len, unsigned rate)
{
    if (len < 10 || len > MAXF - 4) return RF_ERR_RANGE;
    uint8_t *f = rf_wifi_tx_buffer();
    if (frame != f) memcpy(f, frame, len);
    uint32_t c = crc32_le(0, f, len);                        /* the hardware does not append the FCS (docs/WIFI_MAC.md) */
    for (int i = 0; i < 4; i++) f[len + i] = (uint8_t)(c >> 8 * i);
    /* No hardware ACK: the sender repeats a frame to us for a few ms. Sending in the middle of that only collides with it (measured: 4 of 20 peer pings answered). */
    while (time_us() - last_mine < WIFI_QUIET_US) {                  /* watch the air meanwhile: each repeat of a frame to us restarts the wait (the repeats are not delivered again) */
        uint8_t drop[16];                                  /* only the addresses are needed */
        if (rf_wifi_rx(drop, sizeof drop, 0) >= 10 && !memcmp(drop + 4, my, 6)) last_mine = time_us();
    }
    rf_wifi_set_rate(rate);
    int rc = rf_wifi_tx_init();
    if (!rc) { delay_us(1000); rc = rf_wifi_tx(f, len + 4); }   /* the TX front end needs ~1 ms to settle (measured) */
    if (!rc) delay_us(192 + 8 * (len + 4) / mbps(rate) + 100);        /* the done flag comes before the frame has left the air (long DSSS preamble as an upper bound) */
    int rl = listen();
    if (rc || rl) { n_fail++; return rc ? rc : rl; }
    n_tx++;
    return RF_OK;
}

int wifi_mac_rx(wifi_rx_t *r, uint32_t timeout_ms)
{
    uint32_t t0 = ccount();
    do {
        int l = rf_wifi_rx(buf, sizeof buf, 1);
        if (l <= 0) continue;
        const uint32_t *h = (const uint32_t *)rf_wifi_rx_header();
        /* The delivered frame ends in 4 bytes that are not a CRC-32 of it (measured). Noise the receiver locked on to is flagged in header word 5:
         * every real frame has bits 23:8 clear, the 32-byte "fc 94" junk and truncated frames do not (81 frames: 41 / 40 split, no real frame lost). */
        if (l < 14 || (h[5] & 0x00FFFF00u)) { n_drop++; if (l >= 10 && !memcmp(buf + 4, my, 6)) n_drop_mine++; continue; }
        r->data = buf; r->len = (uint16_t)(l - 4); r->rssi = (int8_t)(h[0] & 0xFF); r->rate = (h[0] >> 8) & 0x1F; r->chan = chan;
        n_rx++;
        if (!memcmp(buf + 4, my, 6)) { n_mine++; if (buf[1] & 8) n_retry++; last_mine = time_us(); }
        return 1;
    } while ((ccount() - t0) / 240000u < timeout_ms);
    return 0;
}

void wifi_mac_stats(uint32_t *rx, uint32_t *rx_drop, uint32_t *tx, uint32_t *tx_fail)
{
    if (rx) *rx = n_rx;
    if (rx_drop) *rx_drop = n_drop;
    if (tx) *tx = n_tx;
    if (tx_fail) *tx_fail = n_fail;
}
