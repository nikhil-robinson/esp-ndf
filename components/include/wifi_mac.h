#pragma once
#include <stdint.h>

/* Half-duplex 802.11 MAC on the S3 baseband without the vendor stack (docs/WIFI_MAC.md). The radio listens; a transmit switches to TX
 * (about 2.3 ms with the settle time), sends the frame and switches back. No hardware ACK yet: peers retransmit unacknowledged frames
 * (retry bit), the layers above drop duplicates. Frames are given without FCS (this layer adds it: the TX hardware does not). Received frames are delivered without their 4 trailing bytes
 * (not a CRC-32), after the receiver's noise flag in the header has been checked; there is no FCS check on receive. */
typedef struct { const uint8_t *data; uint16_t len; int8_t rssi; uint8_t rate; uint8_t chan; uint8_t hw_decrypted; } wifi_rx_t;   /* hw_decrypted: protected frame already decrypted and MIC-checked by the MAC: CCMP header still in place, MIC gone */

int  wifi_mac_init(const uint8_t mac[6]);                   /* rf_init done by the caller; starts listening on channel 6 */
int  wifi_mac_set_channel(uint8_t ch);                      /* 1..13 */
#define WIFI_RATE_1M  0x0                                   /* IDF wifi_phy_rate_t codes; measured: 6M 0xB, 9M 0xF, 12M 0xA, 18M 0xE, 24M 0x9, 36M 0xD work; 48M 0x8 and 54M 0xC are not heard */
#define WIFI_RATE_6M  0xB
#define WIFI_RATE_12M 0xA
int  wifi_mac_tx(const uint8_t *frame, unsigned len);       /* frame without FCS (10..1596 B), 1 Mbit/s (DSSS frames over ~1200 B are not heard: use an OFDM rate); 0 when it has been sent, RF_ERR_* otherwise */
int  wifi_mac_tx_rate(const uint8_t *frame, unsigned len, unsigned rate);
int  wifi_mac_rx(wifi_rx_t *r, uint32_t timeout_ms);        /* 1 = frame with good FCS (valid until the next call), 0 = none */
void wifi_mac_stats(uint32_t *rx, uint32_t *rx_drop, uint32_t *tx, uint32_t *tx_fail);   /* rx_drop = flagged as noise / too short */

/* Hardware CCMP receive. Keys for frames from ta: kind 0 = pairwise (key id 0), kind 1 = group (key id as in the CCMP header). Frames the MAC cannot
 * verify are discarded by it. Transmit stays in software (the FCS and CCMP are built by the caller). */
void wifi_mac_set_key(unsigned kind, const uint8_t ta[6], const uint8_t key[16], unsigned key_id);
void wifi_mac_clear_key(unsigned kind);                     /* kinds 0..7: the access point keeps one pairwise key per station (key id 0) */
void wifi_mac_clear_keys(void);
