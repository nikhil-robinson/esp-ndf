/* Wi-Fi MAC driver v2: FCS in both directions, TX/RX switching, counters (see wifi_mac.h). */
#include <string.h>
#include "wifi_mac.h"
#include "rf.h"
#include "crc32.h"
#include "sys.h"
#include "xtensa.h"

#define MAXF 1600
#define R32(a) (*(volatile uint32_t *)(a))

static uint8_t my[6], chan = 6, buf[MAXF];
static uint32_t n_rx, n_drop, n_tx, n_fail;

static int listen(void)                                      /* RX front end + ring + address filter (the register replay overwrites it) */
{
    int rc = rf_wifi_rx_init(); if (rc) return rc;
    R32(0x60033040) = my[0] | my[1] << 8 | my[2] << 16 | (uint32_t)my[3] << 24; R32(0x60033044) = my[4] | my[5] << 8;
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

int wifi_mac_tx(const uint8_t *frame, unsigned len)
{
    if (len < 10 || len > MAXF - 4) return RF_ERR_RANGE;
    static uint8_t f[MAXF];
    memcpy(f, frame, len);
    uint32_t c = crc32_le(0, f, len);                        /* the hardware does not append the FCS (docs/WIFI_MAC.md) */
    for (int i = 0; i < 4; i++) f[len + i] = (uint8_t)(c >> 8 * i);
    int rc = rf_wifi_tx_init();
    if (!rc) { delay_us(1000); rc = rf_wifi_tx(f, len + 4); }   /* the TX front end needs ~1 ms to settle (measured) */
    if (!rc) delay_us(192 + 8 * (len + 4) + 100);            /* the done flag comes before the frame has left the air: 1 Mbit/s, long preamble */
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
        if (l < 14 || (h[5] & 0x00FFFF00u)) { n_drop++; continue; }
        r->data = buf; r->len = (uint16_t)(l - 4); r->rssi = (int8_t)(h[0] & 0xFF); r->rate = (h[0] >> 8) & 0x1F; r->chan = chan;
        n_rx++;
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
