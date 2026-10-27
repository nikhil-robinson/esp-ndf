/* 4.2: raw 802.11 frames through the Wi-Fi MAC, driven entirely from our own code
 * (no vendor stack). Channel 6 (2437 MHz). The absolute baseband / front-end / MAC
 * register state was captured from a reference transmit and receive and lives in
 * rf_wifi_regs.h; rf_set_freq_khz + rf_tx_start/rf_rx_start bring up our calibrated
 * carrier and front end. See docs/PENDING.md and bench/rfmeas/wifi_tx_42.txt. */
#include "rf.h"
#include "xtensa.h"
#include <string.h>
#include "rf_init.h"
#include "rf_wifi_regs.h"

static uint32_t ch_khz = 2437000u;                           /* channel 6 until rf_wifi_set_channel */
#define WIFI_CH_KHZ   ch_khz
#define WRX_N         8
#define WRX_BUF       1664
#define WRX_OWNED     (0x80640000u | WRX_BUF)   /* owner (bit31) | buffer size (low 16) */
#define WRX_HDR       48                        /* rx control header before the MAC frame */

extern void rf_tone_off(void);

static __attribute__((aligned(16))) uint8_t  tx_frame[1600];
static __attribute__((aligned(16))) uint32_t tx_desc[4];
static __attribute__((aligned(16))) uint32_t rx_desc[WRX_N][4];
static __attribute__((aligned(16))) uint8_t  rx_buf[WRX_N][WRX_BUF];

static uint8_t last_hdr[WRX_HDR];
static uint32_t last_desc[4];
const uint32_t (*rf_wifi_rx_ring(void))[4] { return rx_desc; }   /* the 4 RX descriptors (debug) */
const uint32_t *rf_wifi_rx_desc(void) { return last_desc; }   /* descriptor words of the frame rf_wifi_rx returned last */
const uint8_t *rf_wifi_rx_header(void) { return last_hdr; }   /* the 48-byte MAC receive header of the frame rf_wifi_rx returned last */

/* The TX and RX register sets each leave state the other does not rewrite; after a TX replay the receiver stays dead even when the RX set is
 * replayed again (measured). Pulsing the MAC reset (SYSTEM_WIFI_RST bit 2: MAC only, the RF/baseband setup stays) before each replay fixes it. */
static void mac_reset(void) { *(volatile uint32_t *)0x60026018 |= 4; *(volatile uint32_t *)0x60026018 &= ~4u; }

static void replay(const rf_reg_t *r, unsigned n)
{
    for (unsigned i = 0; i < n; i++) *(volatile uint32_t *)r[i].addr = r[i].value;
}

int rf_wifi_set_channel(unsigned ch) { if (ch < 1 || ch > 13) return RF_ERR_RANGE; ch_khz = 2412000u + 5000u * (ch - 1); return RF_OK; }   /* takes effect at the next tx_init / rx_init */

int rf_wifi_tx_init(void)
{
    int rc = rf_set_freq_khz(WIFI_CH_KHZ); if (rc) return rc;
    rc = rf_tx_start(); if (rc) return rc;
    rf_tone_off();                                           /* free the DAC path for the baseband modem */
    mac_reset();
    replay(rf_wifi_tx_regs, sizeof rf_wifi_tx_regs / sizeof rf_wifi_tx_regs[0]);
    return RF_OK;
}

static uint8_t tx_rate;
void rf_wifi_set_rate(unsigned r) { tx_rate = (uint8_t)r; }

uint8_t *rf_wifi_tx_buffer(void) { return tx_frame; }       /* build the frame here and pass the same pointer to rf_wifi_tx: no copy */

int rf_wifi_tx(const uint8_t *frame, unsigned len)
{
    if (len < 10 || len > sizeof tx_frame) return RF_ERR_RANGE;
    if (frame != tx_frame) memcpy(tx_frame, frame, len);
    tx_desc[0] = 0xC0000000u | ((uint32_t)len << 12) | len;  /* owner | EOF | size<<12 | len */
    tx_desc[1] = (uint32_t)tx_frame;
    tx_desc[2] = (uint32_t)tx_desc;                          /* next: self */
    tx_desc[3] = 0;
    *(volatile uint32_t *)0x600342f8 = len | (uint32_t)tx_rate << 12;   /* PLCP1: length, rate (0 = 1 Mbit/s) */
    *(volatile uint32_t *)0x60034300 = 0x6400;               /* HTSIG */
    *(volatile uint32_t *)0x60033d04 = 0x1000;               /* CONF: TX enable */
    *(volatile uint32_t *)0x60033c34 |= 2;
    *(volatile uint32_t *)0x60033c40 |= 3;
    *(volatile uint32_t *)0x60033d08 = ((uint32_t)tx_desc & 0xFFFFF) | 0xC0000000u;  /* PLCP0: desc + start */
    uint32_t t0 = ccount();
    while (!(*(volatile uint32_t *)0x60033c3c & 2) && ccount() - t0 < 240000000u / 100) {}
    return (*(volatile uint32_t *)0x60033c3c & 2) ? RF_OK : RF_ERR_TIMEOUT;
}

int rf_wifi_rx_init(void)
{
    int rc = rf_set_freq_khz(WIFI_CH_KHZ); if (rc) return rc;
    rc = rf_rx_start(); if (rc) return rc;
    mac_reset();
    replay(rf_wifi_rx_regs, sizeof rf_wifi_rx_regs / sizeof rf_wifi_rx_regs[0]);
    for (int i = 0; i < WRX_N; i++) {
        rx_desc[i][0] = rx_desc[i][3] = WRX_OWNED;
        rx_desc[i][1] = (uint32_t)rx_buf[i];
        rx_desc[i][2] = (uint32_t)rx_desc[(i + 1) % WRX_N];
    }
    uint32_t head = (uint32_t)rx_desc[0] & 0xFFFFF;          /* 20-bit DRAM offset */
    for (uint32_t reg = 0x60033088; reg <= 0x60033098; reg += 4) *(volatile uint32_t *)reg = head;
    return RF_OK;
}

/* Poll our RX ring; on the first frame the MAC delivered, copy the 802.11 frame (control
 * header stripped) into out (up to max), re-arm the descriptor, and return its length.
 * 0 on timeout. */
int rf_wifi_rx(uint8_t *out, unsigned max, uint32_t timeout_ms)
{
    uint32_t t0 = ccount();
    while ((ccount() - t0) / 240000u < timeout_ms) {
        for (int i = 0; i < WRX_N; i++) {
            if (!(rx_desc[i][0] & 0x40000000u)) continue;                             /* bit 30 (has data) is set by the MAC when the frame is complete */
            unsigned len = *(volatile uint32_t *)(rx_buf[i] + 44) & 0xFFF, dl = (rx_desc[i][0] >> 12) & 0xFFF;   /* frame length from the header; buffer length from the descriptor */
            if (dl < len + 44 || dl > len + 46) { rx_desc[i][0] = rx_desc[i][3] = WRX_OWNED; continue; }          /* no MAC header: a continuation buffer (descriptor length = frame + 45, +-1 measured) */
            memcpy(last_desc, rx_desc[i], sizeof last_desc);
            if (len > WRX_BUF - WRX_HDR) len = WRX_BUF - WRX_HDR;
            if (len > max) len = max;
            memcpy(last_hdr, rx_buf[i], WRX_HDR);
            memcpy(out, rx_buf[i] + WRX_HDR, len);
            rx_desc[i][0] = rx_desc[i][3] = WRX_OWNED;                                /* re-arm */
            return (int)len;
        }
    }
    return 0;
}
