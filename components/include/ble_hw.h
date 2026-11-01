/* Hardware BLE receive on the ESP32-S3's own link-layer core, no vendor code.
 * The core demodulates, checks the access address and CRC, dewhitens and writes
 * each PDU to exchange memory by itself; the CPU only re-arms one scan event per
 * received packet (an interrupt) and copies the PDU out. See docs/PHY_STANDALONE.md. */
#pragma once
#include <stdint.h>

typedef struct {
    uint8_t  hdr0, len;        /* PDU header: type [3:0], TxAdd [6], RxAdd [7]; payload length */
    uint8_t  chan;             /* advertising channel index (37..39) */
    int8_t   rssi;             /* dBm, as the core reports it */
    uint32_t clkn;             /* core clock at sync, half-slots (312.5 us) */
    uint8_t  data[255];        /* payload (AdvA first for advertising PDUs; a connection's data PDU up to 251 bytes after the data length update) */
} ble_hw_pkt_t;

typedef struct {
    uint32_t events, packets, dropped;   /* scan events run, PDUs received, PDUs lost to a full queue */
    uint32_t aborts, isr_calls, skips, disconnects, tx_pdus, rx_errors;                     /* stuck events aborted by the watchdog */
    uint32_t isr_cycles;                 /* CPU cycles spent in the interrupt handler */
    uint32_t tx_acked;                   /* data PDUs the core finished sending (descriptor done) */
} ble_hw_stats_t;

int  ble_hw_init(void);                /* after rf_init_replay(); 0 on success */
void ble_hw_core_reset(void);          /* core reset + reload (clears the coded-timeout no-sync state); raw engine stopped */
void ble_hw_scan(uint8_t chan);        /* continuous passive scan on one advertising channel (37, 38, 39) */
void ble_hw_stop(void);
/* Advertise ADV_NONCONN_IND (AdvA + up to 31 bytes AdvData) from the core every interval_us on one channel. */
void ble_hw_adv_type(uint8_t type);   /* before ble_hw_adv: 0 ADV_IND (connectable), 2 ADV_NONCONN_IND (default) */
void ble_hw_set_adva(const uint8_t adva[6]);                 /* change the advertiser address of a running advertisement (random type stays) */
int  ble_hw_adv(uint8_t chan, const uint8_t adva[6], int random, const uint8_t *data, unsigned len, uint32_t interval_us);
int  ble_hw_read(ble_hw_pkt_t *p);     /* 1 and the oldest PDU, or 0 when none; also services the core */
int  ble_hw_wait(ble_hw_pkt_t *p, uint32_t timeout_us);   /* sleep (waiti) until a PDU or the timeout */
void ble_hw_get_stats(ble_hw_stats_t *s);
/* Connection (slave): after ble_hw_adv_type(0) + ble_hw_adv(), a CONNECT_IND to our address starts the link.
 * Received LL data PDUs arrive through ble_hw_read (hdr0 = LL header byte 0: LLID [1:0]). */
int  ble_hw_connected(void);
void ble_hw_phy(uint8_t *tx, uint8_t *rx);                  /* PHY of the connection, 0 = 1M, 1 = 2M */
void ble_hw_csa2(int on);              /* default on: ChSel set, the central may pick CSA#2; 0 forces CSA#1 */
int  ble_hw_conn_info(uint8_t *csa2, uint16_t *interval);   /* channel selection in use, interval */
void ble_hw_conn_addrs(uint8_t peer[7], uint8_t own[7]);        /* the connection's peer and our address: type (0 public, 1 random) then 6 octets, least significant first */
void ble_hw_set_enc(int (*ltk)(const uint8_t rand[8], uint16_t ediv, uint8_t ltk[16]), void (*done)(int on));   /* link-layer encryption: key lookup for LL_ENC_REQ (0 = found), and a callback once both directions are encrypted */
unsigned ble_hw_tx_max(void);           /* largest LL data PDU payload we send: 27, or up to 251 once the central's LL_LENGTH_REQ is accepted and the TX buffers are switched */
int  ble_hw_tx_free(void);              /* free slots in the TX queue */
int  ble_hw_send(uint8_t llid, const uint8_t *data, unsigned len);   /* llid 2 L2CAP start, 1 continuation, 3 LL control; <= ble_hw_tx_max() bytes */
int  ble_hw_ll_control(const ble_hw_pkt_t *p);                       /* answers LL control PDUs; 1 if consumed */
void ble_hw_debug(uint32_t out[6]);         /* intmask, raw status, ET0 word, clkn, RX desc ptr, ET0 start */
/* raw packet engine (core test mode, no BLE protocol): continuous packets of preamble + sync + len bytes + CRC.
 * ch_idx: BLE channel index (37 = 2402 MHz, 0..36 = 2404..2478, 38 = 2426, 39 = 2480); rate 0 1M, 1 2M, 2 coded S8, 3 coded S2;
 * type: payload source (0 PRBS9, 2 1010..., others: see docs). */
int  ble_hw_raw_tx(uint8_t ch_idx, uint8_t rate, uint32_t sync, uint32_t crcinit, uint8_t type, const uint8_t *d, unsigned len, uint8_t txpwr);
int  ble_hw_raw_send(uint8_t ch_idx, uint8_t rate, uint32_t sync, uint32_t crcinit, uint8_t hdr0, const uint8_t *d, unsigned len);
int  ble_hw_raw_sendn(uint8_t ch_idx, uint8_t rate, uint32_t sync, uint32_t crcinit, uint8_t hdr0, const uint8_t *d, unsigned len, uint32_t count);
int  ble_hw_raw_read2(uint8_t *buf, unsigned max, unsigned *len, uint16_t *status, int8_t *rssi, uint8_t *hdr);
int  ble_hw_raw_busy(void);
int  ble_hw_raw_read3(uint8_t *buf, unsigned max, unsigned *len, uint16_t *status, int8_t *rssi, uint8_t *hdr, uint64_t *t_hu);
uint64_t ble_hw_now_hu(void);
int  ble_hw_raw_send_at(uint8_t ch_idx, uint8_t rate, uint32_t sync, uint32_t crcinit, uint8_t hdr0, const uint8_t *d, unsigned len, uint64_t t_hu);
extern uint32_t ble_hw_raw_flags;
int  ble_hw_raw_rx(uint8_t ch_idx, uint8_t rate, uint32_t sync, uint32_t crcinit);
int  ble_hw_raw_read(uint8_t *buf, unsigned max, unsigned *len, uint16_t *status, int8_t *rssi);
uint32_t ble_hw_raw_rxcount(void);
int  ble_hw_raw_watchdog(uint32_t max_ms);
void ble_hw_raw_stop(void);
uint32_t ble_hw_raw_txcount(void);
uint16_t ble_hw_em_rd(uint32_t off);
void ble_hw_em_wr(uint32_t off, uint16_t v);
