/* Wi-Fi MAC driver v2: FCS in both directions, TX/RX switching, counters (see wifi_mac.h). */
#include <string.h>
#include "wifi_mac.h"
#include "rf.h"
#include "crc32.h"
#include "sys.h"
#include "xtensa.h"

#define MAXF 1600
#ifndef WIFI_TX_TRIES
#define WIFI_TX_TRIES 7                                  /* attempts per unicast frame until the ACK comes (IDF: short retry limit 7) */
#endif
#define R32(a) (*(volatile uint32_t *)(a))

static uint8_t my[6], chan = 6, buf[MAXF];
uint32_t n_drop_mine, n_mine, n_retry, n_retries_tx, n_noack, n_cwb40, n_agg, n_ampdu, n_mine40, n_retry40;   /* n_mine40 / n_retry40: frames to us that were 40 MHz wide, and those with the retry bit */   /* n_agg: frames that were part of an A-MPDU (word 1 bit 27, IDF aggregation) */   /* n_cwb40: frames received 40 MHz wide (control word 1 bit 7, IDF cwb) */
static uint32_t n_rx, n_drop, n_tx, n_fail;

#define NKEYS 8                                              /* key-table slots 4..4+NKEYS-1: the station uses two, the access point one per station */
static struct { uint8_t ta[6], key[16], id, used; } keys[NKEYS];
static void install_keys(void);

static int listen(void)                                      /* RX front end + ring + address filter (the register replay overwrites it) */
{
    int rc = rf_wifi_rx_init(); if (rc) return rc;
    R32(0x60033040) = my[0] | my[1] << 8 | my[2] << 16 | (uint32_t)my[3] << 24; R32(0x60033044) = my[4] | my[5] << 8;
    R32(0x60033060) = 0xFFFFFFFFu; R32(0x60033064) = 0x0001FFFFu;                 /* match all 48 bits of slot 0, filter enabled (bit 16) */
    R32(0x60033810) &= 0xFFC0003Fu;                             /* the replayed set has bits 21:6 set: the MAC would consume protected unicast frames that no key-table entry decrypts (the AP decrypts in software) */
    install_keys();                                             /* the MAC reset in rf_wifi_rx_init cleared the key table */
    return RF_OK;
}

int wifi_mac_init(const uint8_t mac[6])
{
    memcpy(my, mac, 6);
    int rc = rf_wifi_set_channel(chan); if (rc) return rc;
    return listen();
}

int wifi_mac_set_channel(uint8_t ch) { return wifi_mac_set_channel_bw(ch, 0); }
int wifi_mac_set_channel_bw(uint8_t ch, uint8_t sec40)
{
    int rc = rf_wifi_set_channel(ch); if (rc) return rc;
    rc = rf_wifi_set_bw(sec40); if (rc) return rc;
    chan = ch;
    return listen();
}


int wifi_mac_tx(const uint8_t *frame, unsigned len) { return wifi_mac_tx_rate(frame, len, WIFI_RATE_1M); }

static int tx_frame(const uint8_t *frame, unsigned len, unsigned rate, unsigned hw);
int wifi_mac_tx_rate(const uint8_t *frame, unsigned len, unsigned rate) { return tx_frame(frame, len, rate, 0); }
int wifi_mac_tx_hwcrypt(const uint8_t *frame, unsigned len, unsigned rate) { return tx_frame(frame, len, rate, RF_TX_HWCRYPT); }
static int tx_frame(const uint8_t *frame, unsigned len, unsigned rate, unsigned hw)
{
    if (len < 10 || len > MAXF - 4) return RF_ERR_RANGE;
    uint8_t *f = rf_wifi_tx_buffer();
    if (frame != f) memcpy(f, frame, len);
    uint32_t c = crc32_le(0, f, len);                        /* the hardware does not append the FCS (docs/WIFI_MAC.md) */
    for (int i = 0; i < 4; i++) f[len + i] = (uint8_t)(c >> 8 * i);
    /* Frame-exchange engine (IDF's flow): carrier sense, ACK wait and retries with the retry bit; the radio stays in its receive set, so a received
     * frame's hardware ACK is never cut off and our own ACKs keep going out while we send. */
    rf_wifi_set_rate(rate);
    int group = f[4] & 1, r = 0;
    for (int tries = 0; tries < WIFI_TX_TRIES; tries++) {
        r = rf_wifi_tx_ack(f, len + 4, (group ? 0 : RF_TX_ACK) | hw);
        if (r != 0) break;
        f[1] |= 0x08; c = crc32_le(0, f, len); for (int i = 0; i < 4; i++) f[len + i] = (uint8_t)(c >> 8 * i);
        n_retries_tx++;
    }
    if (r < 0) { n_fail++; return r; }
    if (r == 0) { n_noack++; return RF_ERR_TIMEOUT; }
    n_tx++;
    return RF_OK;
}

int wifi_mac_tx_ampdu(const uint8_t *const *f, const uint16_t *len, unsigned n, unsigned rate, uint16_t *ssc, uint32_t bitmap[2])
{
    int p = rf_wifi_agg_prepare(f, len, n);                  /* the MAC adds delimiters, padding and FCS itself (rf_wifi.c) */
    if (p < 0) return p;
    rf_wifi_set_rate(rate);
    int r = rf_wifi_tx_ack(NULL, 0, RF_TX_ACK | RF_TX_AGG);
    if (r < 0) { n_fail++; return r; }
    if (r == 0) { n_noack++; return RF_ERR_TIMEOUT; }
    n_tx++; n_ampdu++; rf_wifi_ba(ssc, bitmap);
    return RF_OK;
}

int wifi_mac_rx(wifi_rx_t *r, uint32_t timeout_ms)
{
    uint32_t t0 = ccount();
    do {
        int l = rf_wifi_rx(buf, sizeof buf, 1);
        if (l <= 0) continue;
        const uint32_t *h = (const uint32_t *)rf_wifi_rx_header();
        /* Receive control header as IDF's wifi_pkt_rx_ctrl_t (esp_wifi_types_native.h, ESP32-S3): word 0 rssi / rate / sig_mode, word 11 sig_len (bits 11:0)
         * and rx_state (bits 31:24, 0 = no error). IDF delivers only rx_state 0; noise and broken frames have it set. (The old test on word 5 was the
         * noise floor and dropped every OFDM frame.) */
        if (l < 14 || (h[11] >> 24)) { n_drop++; if (l >= 10 && !memcmp(buf + 4, my, 6)) n_drop_mine++; continue; }
        r->data = buf; r->len = (uint16_t)(l - 4); r->rssi = (int8_t)(h[0] & 0xFF); r->rate = (h[0] >> 8) & 0x1F; r->chan = chan; r->hw_decrypted = (uint8_t)rf_wifi_rx_decrypted();
        n_rx++; n_cwb40 += (h[1] >> 7) & 1; n_agg += (h[1] >> 27) & 1;
        if (!memcmp(buf + 4, my, 6)) { n_mine++; if (buf[1] & 8) n_retry++; if ((h[1] >> 7) & 1) { n_mine40++; if (buf[1] & 8) n_retry40++; } }
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

/* Key table, as IDF's hal_crypto_* program it (read from the library's disassembly) and as measured with host-encrypted frames (docs/WIFI_MAC.md):
 * slot n at 0x60034400 + 0x28 * n = transmitter address, a flag halfword, the 16-byte key at +8, the other words zero; 0x60033814 has one enable
 * bit per slot; 0x60033800 (interface 0) = 0x30103; 0x60033810 bits 21:6 as IDF has them (IDF also sets 0x600330d8 bit 1; that bit stops the hardware ACK, so it stays clear). Measured: slots 0-3 never decrypt
 * CCMP, slots 4 and up do; flag halfword 0x086c | key id << 8 decrypts and verifies unicast and group frames (the pairwise flags IDF uses for its
 * transmit key, 0x48cc, make the MAC abort the frame); the highest matching slot wins, so the key id (pairwise 0, group 1..3) tells the two apart. */
static void install_keys(void)
{
    uint32_t en = 0;
    for (unsigned n = 0; n < NKEYS; n++) {
        if (!keys[n].used) continue;
        unsigned slot = 4 + n;
        volatile uint32_t *k = (volatile uint32_t *)(0x60034400u + 0x28u * slot); const uint8_t *ta = keys[n].ta, *key = keys[n].key;
        for (int i = 0; i < 10; i++) k[i] = 0;
        k[0] = ta[0] | ta[1] << 8 | ta[2] << 16 | (uint32_t)ta[3] << 24;
        k[1] = ta[4] | ta[5] << 8 | (uint32_t)(0x086c | (keys[n].id & 3) << 8) << 16;
        for (int i = 0; i < 4; i++) k[2 + i] = key[4 * i] | key[4 * i + 1] << 8 | key[4 * i + 2] << 16 | (uint32_t)key[4 * i + 3] << 24;
        en |= 1u << slot;
        if (!keys[n].id && n < 4) {                              /* pairwise: a transmit twin in slot n (0-3) with IDF's transmit flags 0x48cc (IDF's connected key table) */
            volatile uint32_t *t = (volatile uint32_t *)(0x60034400u + 0x28u * n);
            for (int i = 0; i < 10; i++) t[i] = k[i];
            t[1] = (k[1] & 0xFFFF) | 0x48ccu << 16;
            en |= 1u << n;
        }
    }
    if (en) { R32(0x60033810) &= 0xFFC0003Fu; R32(0x60033800) = (R32(0x60033800) | 0x30103u) & 0x3FFFFFFFu; }
    R32(0x60033814) = en;
}
void wifi_mac_set_key(unsigned kind, const uint8_t ta[6], const uint8_t key[16], unsigned key_id)
{
    if (kind >= NKEYS) return;
    memcpy(keys[kind].ta, ta, 6); memcpy(keys[kind].key, key, 16); keys[kind].id = (uint8_t)key_id; keys[kind].used = 1;
    install_keys();
}
void wifi_mac_clear_key(unsigned kind) { if (kind < NKEYS) { volatile uint8_t *z = (volatile uint8_t *)&keys[kind]; for (unsigned i = 0; i < sizeof keys[kind]; i++) z[i] = 0; install_keys(); } }
void wifi_mac_clear_keys(void)
{
    volatile uint8_t *z = (volatile uint8_t *)keys; for (unsigned i = 0; i < sizeof keys; i++) z[i] = 0;
    R32(0x60033814) = 0; R32(0x60033800) = 0x30000;
}

/* Receive block-ack sessions: 8 register blocks at 0x60033274 - 36 * i (IDF hal_agreement_add_rx_ba / _del_rx_ba, bench/wifi_regs/data/idf_hal_rx_ba.dis):
 * +0 control (tid << 12 | 0xc0000005 = active; bit 31 stops updates while the block is rewritten), +4 / +8 peer address, +12 window size << 13 | start sequence,
 * +16 / +20 the received-frame bitmap the MAC answers block-ack requests and A-MPDUs from */
#define BA(i) (0x60033274u - 36u * (i))
void wifi_mac_ba_add(unsigned i, const uint8_t ta[6], unsigned tid, unsigned ssn, unsigned win)
{
    if (i >= 8) return;
    R32(BA(i) + 8) = ta[0] | ta[1] << 8 | ta[2] << 16 | (uint32_t)ta[3] << 24;
    R32(BA(i) + 4) = ta[4] | ta[5] << 8;
    R32(BA(i) + 12) = (win & 127) << 13 | (ssn & 0xFFF);
    R32(BA(i)) &= 0x7FFFFFFFu; R32(BA(i) + 16) = 0; R32(BA(i) + 20) = 0;
    R32(BA(i)) = (tid << 12 & 0xFFFF) | 0xC0000005u;
}
void wifi_mac_ba_del(unsigned i)
{
    if (i >= 8) return;
    R32(BA(i)) &= 0x7FFFFFFFu; R32(BA(i) + 16) = 0; R32(BA(i) + 20) = 0; R32(BA(i)) |= 0x80000000u; R32(BA(i)) = 0;
}
