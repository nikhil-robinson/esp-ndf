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
    uint8_t  data[37];         /* payload (AdvA first for advertising PDUs) */
} ble_hw_pkt_t;

typedef struct {
    uint32_t events, packets, dropped;   /* scan events run, PDUs received, PDUs lost to a full queue */
    uint32_t aborts, isr_calls;                     /* stuck events aborted by the watchdog */
    uint32_t isr_cycles;                 /* CPU cycles spent in the interrupt handler */
} ble_hw_stats_t;

int  ble_hw_init(void);                /* after rf_init_replay(); 0 on success */
void ble_hw_scan(uint8_t chan);        /* continuous passive scan on one advertising channel (37, 38, 39) */
void ble_hw_stop(void);
int  ble_hw_read(ble_hw_pkt_t *p);     /* 1 and the oldest PDU, or 0 when none; also services the core */
int  ble_hw_wait(ble_hw_pkt_t *p, uint32_t timeout_us);   /* sleep (waiti) until a PDU or the timeout */
void ble_hw_get_stats(ble_hw_stats_t *s);
void ble_hw_debug(uint32_t out[6]);         /* intmask, raw status, ET0 word, clkn, RX desc ptr, ET0 start */
