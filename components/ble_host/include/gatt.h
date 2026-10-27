/* GATT server over the S3's own BLE link layer (ble_hw.h): attribute table, ATT requests, notifications. ATT MTU is 23 (one LL
 * data PDU). UUIDs are 16-bit. att.c is pure (no radio): att_handle() is what the host fuzzer and tests drive. */
#pragma once
#include <stdint.h>

#ifndef NDF_GATT_MAX_ATTRS
#define NDF_GATT_MAX_ATTRS 64              /* attributes in the table (service = 1, characteristic = 2, + 1 with CCCD) */
#endif
#define GATT_MTU 23

#define GATT_P_READ     0x02
#define GATT_P_WRITE_NR 0x04
#define GATT_P_WRITE    0x08
#define GATT_P_NOTIFY   0x10
#define GATT_P_INDICATE 0x20

typedef struct {
    uint16_t uuid;                                              /* 16-bit characteristic UUID */
    uint8_t  props;                                             /* GATT_P_* */
    int (*read)(uint8_t *out, uint16_t max);                    /* value into out (<= max); returns length, < 0 = ATT error code negated */
    int (*write)(const uint8_t *in, uint16_t n);                /* 0 ok, < 0 = ATT error code negated */
    uint16_t handle;                                            /* set by gatts_add_service: the value handle (CCCD = handle + 1) */
} gatt_chr_t;

void gatt_db_clear(void);
int  gatts_add_service(uint16_t uuid16, gatt_chr_t *chrs, unsigned n);        /* 0 ok, -1 table full; handles are assigned in call order */
int  att_handle(const uint8_t *pdu, unsigned n, uint8_t *rsp, unsigned max);   /* one ATT PDU in, response out (0 = none); max >= GATT_MTU */
void gatt_reset(void);                                                          /* new connection: subscriptions off */
int  gatts_subscribed(const gatt_chr_t *c);                                     /* bit 0 notifications, bit 1 indications */
int  att_indication_pending(void);
void att_indication_start(void);                                                /* gap.c: an indication has been sent */                                              /* an indication awaits its confirmation */

/* radio side (gap.c): */
int  gatts_init(const char *device_name);                                       /* GAP (0x1800: device name) + GATT (0x1801) services; call before your own */
int  gatts_advertise(const uint8_t addr[6], uint32_t interval_us);              /* connectable advertising of the device name; re-advertises by itself after a disconnect */
void gatt_poll(uint32_t wait_us);                                               /* main loop: sleep up to wait_us for a PDU, then drain the link layer: ATT, LL control, L2CAP signalling */
int  gatts_notify(const gatt_chr_t *c, const uint8_t *v, unsigned n);          /* 0 sent; < 0 not connected / not subscribed / n > 20 / link busy */
int  gatts_indicate(const gatt_chr_t *c, const uint8_t *v, unsigned n);        /* as notify, one in flight until the central confirms */
