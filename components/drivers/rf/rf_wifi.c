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
#ifndef NDF_WIFI_RX_BUFS
#define NDF_WIFI_RX_BUFS 11
#endif
#define WRX_N         NDF_WIFI_RX_BUFS            /* receive ring: frames the MAC can take while the CPU is busy (at 11 Mbit/s a burst of the AP overflowed 8 and cost a TCP retransmit timeout; 11: none in 24 runs) */
#define WRX_BUF       1664
#define WRX_OWNED     (0x80640000u | WRX_BUF)   /* owner (bit31) | buffer size (low 16) */
#define WRX_HDR       48                        /* rx control header before the MAC frame */

extern void rf_tone_off(void);

static __attribute__((aligned(16))) uint8_t  tx_frame[1600];
static __attribute__((aligned(16))) uint32_t tx_desc[4];
static __attribute__((aligned(16))) uint32_t rx_desc[WRX_N][4];
static __attribute__((aligned(16))) uint8_t  rx_buf[WRX_N][WRX_BUF];

static uint8_t last_hdr[WRX_HDR]; static int last_dec; static uint8_t rx_next;
static uint32_t last_desc[4];
const uint32_t (*rf_wifi_rx_ring(void))[4] { return rx_desc; }   /* the 4 RX descriptors (debug) */
const uint32_t *rf_wifi_rx_desc(void) { return last_desc; }   /* descriptor words of the frame rf_wifi_rx returned last */
int rf_wifi_rx_decrypted(void) { return last_dec; }   /* 1 = the MAC decrypted and verified the last frame (CCMP header kept, MIC removed) */
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
    for (volatile uint32_t *q = (volatile uint32_t *)0x600342e0; q < (volatile uint32_t *)0x600343fc; q++) *q = 0;   /* TX parameter RAM: the MAC patches the duration field and more into the frame from it, and it powers up with random contents; the replay sets what is needed */
    replay(rf_wifi_tx_regs, sizeof rf_wifi_tx_regs / sizeof rf_wifi_tx_regs[0]);
    return RF_OK;
}

static uint8_t tx_rate;
#include "wifi_tx_cfg.h"
void rf_wifi_set_rate(unsigned r) { tx_rate = (uint8_t)r; }

uint8_t *rf_wifi_tx_buffer(void) { return tx_frame; }       /* build the frame here and pass the same pointer to rf_wifi_tx: no copy */

/* Transmit through the MAC's frame-exchange engine from the receive state, as IDF does (no TX register replay): the MAC senses the air, sends,
 * and for a unicast frame waits for the ACK itself. Configuration and result bits read from IDF at a unicast start (JTAG, docs/WIFI_MAC.md):
 * start word bits 31:30 = go, 22:21 always, 24 = a response (ACK) is expected; events 0x60033c3c bit 7 = done, bit 14 = response received,
 * bit 19 = response timeout. Returns 1 = acknowledged (or no ACK expected), 0 = no ACK, < 0 = error. */
int rf_wifi_tx_ack(const uint8_t *frame, unsigned len, unsigned flags)
{
    int want_ack = flags & RF_TX_ACK;
    if (len < 10 || len > sizeof tx_frame) return RF_ERR_RANGE;
    if (frame != tx_frame) memcpy(tx_frame, frame, len);
    *(volatile uint32_t *)0x60033d08 = 0;                     /* an older exchange that never finished is dropped */
    *(volatile uint32_t *)0x60033c40 = 0xFFFFFFFFu;           /* events: write-1-to-clear */
    tx_desc[0] = 0xC0000000u | ((uint32_t)len << 12) | len;
    tx_desc[1] = (uint32_t)tx_frame; tx_desc[2] = (uint32_t)tx_desc; tx_desc[3] = 0;
    for (unsigned i = 0; i < sizeof wifi_tx_cfg / sizeof wifi_tx_cfg[0]; i++) *(volatile uint32_t *)wifi_tx_cfg[i][0] = wifi_tx_cfg[i][1];
    *(volatile uint32_t *)0x600342f8 = len | (uint32_t)tx_rate << 12;      /* PLCP1: length, rate */
    *(volatile uint32_t *)0x60034300 = 0x6400;               /* HTSIG */
    uint32_t dur = want_ack ? (tx_rate ? 44u : 314u) : 0;      /* duration (NAV) the MAC writes into the frame: SIFS + ACK (mac_tx_set_duration) */
    *(volatile uint32_t *)0x60034318 = dur | dur << 16;
    uint32_t ic = *(volatile uint32_t *)0x60033800;
    if (!(flags & RF_TX_HWCRYPT)) *(volatile uint32_t *)0x60033800 = ic & ~0x103u;   /* a frame encrypted in software (or plain): the engine must not encrypt it */
    *(volatile uint32_t *)0x60033d08 = ((uint32_t)tx_desc & 0xFFFFF) | 0xC0600000u | (want_ack ? 0x01000000u : 0);
    /* Completion as IDF's lmacProcessTxComplete decides it: 0x60033cb0 bits 3:0 = queues done; the queue's status word (0x60034320 for queue 0)
     * bits 15:12 = result (0 success, else RTS / CTS / ACK timeout / TX error); the done bit is cleared by setting it in 0x60033cac. */
    uint32_t t0 = ccount(), done = 0, st = 0;
    while (ccount() - t0 < 240000u * 30) {                   /* 30 ms: airtime at 1 Mbit/s + backoff + ACK timeout */
        done = *(volatile uint32_t *)0x60033cb0 & 1;
        if (done || (*(volatile uint32_t *)0x60033cac & 0x10000)) break;
    }
    st = (*(volatile uint32_t *)0x60034320 >> 12) & 0xF;
    *(volatile uint32_t *)0x60033cac = done ? 1 : *(volatile uint32_t *)0x60033cac;
    *(volatile uint32_t *)0x60033d08 = 0;                     /* queue idle again */
    *(volatile uint32_t *)0x60033800 = ic;
    *(volatile uint32_t *)0x60033c40 = 0xFFFFFFFFu;
    if (!done) return want_ack ? 0 : RF_ERR_TIMEOUT;          /* no completion: the ACK timed out (or the medium never freed) */
    return st == 0;
}

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
    /* TX parameter RAM: it keeps its contents over resets and the MAC patches every transmitted frame from it. The engine path transmits from this
     * receive state, so it is set here: zero, then the parameter words of the TX register set (queue PLCP / timing defaults). */
    for (volatile uint32_t *q = (volatile uint32_t *)0x600342e0; q < (volatile uint32_t *)0x600343fc; q++) *q = 0;
    for (unsigned i = 0; i < sizeof rf_wifi_tx_regs / sizeof rf_wifi_tx_regs[0]; i++)
        if (rf_wifi_tx_regs[i].addr >= 0x600342e0u && rf_wifi_tx_regs[i].addr < 0x600343fcu) *(volatile uint32_t *)rf_wifi_tx_regs[i].addr = rf_wifi_tx_regs[i].value;
    replay(rf_wifi_rx_regs, sizeof rf_wifi_rx_regs / sizeof rf_wifi_rx_regs[0]);
    for (int i = 0; i < WRX_N; i++) {
        rx_desc[i][0] = rx_desc[i][3] = WRX_OWNED;
        rx_desc[i][1] = (uint32_t)rx_buf[i];
        rx_desc[i][2] = (uint32_t)rx_desc[(i + 1) % WRX_N];
    }
    uint32_t head = (uint32_t)rx_desc[0] & 0xFFFFF;          /* 20-bit DRAM offset */
    rx_next = 0;
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
        for (int k = 0; k < WRX_N; k++) {
            int i = (rx_next + k) % WRX_N;                                            /* in ring order from the oldest: the MAC fills the descriptors in their linked order */
            if (!(rx_desc[i][0] & 0x40000000u)) continue;                             /* bit 30 (has data) is set by the MAC when the frame is complete */
            rx_next = (uint8_t)((i + 1) % WRX_N);
            unsigned len = *(volatile uint32_t *)(rx_buf[i] + 44) & 0xFFF, dl = (rx_desc[i][0] >> 12) & 0xFFF;   /* frame length from the header; buffer length from the descriptor */
            /* A real frame's descriptor length is the frame + 44 rounded up to 4 (156 -> 200, 155 -> 200, 137 -> 184). A protected frame the MAC decrypted
             * (key table, wifi_mac.c) arrives with its 8-byte MIC removed: frame + 36. Anything else is a continuation buffer. */
            int dec = (rx_buf[i][WRX_HDR + 1] & 0x40) && dl >= len + 36 && dl < len + 40;
            if (!dec && (dl < len + 44 || dl >= len + 44 + 8)) { rx_desc[i][0] = rx_desc[i][3] = WRX_OWNED; continue; }
            last_dec = dec; if (dec) len -= 8;
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
