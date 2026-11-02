/* GATT server over the S3's own BLE link layer (ble_hw.h): attribute table, ATT requests, notifications. ATT MTU 23 until the client exchanges it (up to
 * NDF_GATT_MTU); larger PDUs are fragmented / reassembled in L2CAP. UUIDs are 16-bit. att.c is pure (no radio): att_handle() is what the host fuzzer and tests drive. */
#pragma once
#include <stdint.h>
#ifndef BLE_NCONN
#define BLE_NCONN 2                                       /* simultaneous links (as ble_hw.h) */
#endif

#ifndef NDF_GATT_MAX_ATTRS
#define NDF_GATT_MAX_ATTRS 64              /* attributes in the table (service = 1, characteristic = 2, + 1 with CCCD) */
#endif
#ifndef NDF_GATT_MTU
#define NDF_GATT_MTU 247                   /* largest ATT MTU we accept: one LL PDU of 251 bytes holds the 4-byte L2CAP header + 247 */
#endif
#define GATT_MTU NDF_GATT_MTU              /* buffer size of one ATT PDU; the MTU in use is att_mtu() (23 until the client exchanges) */

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
unsigned att_mtu(void);                                                         /* ATT MTU in use on the link att_set_link named */
int  att_cccd(const gatt_chr_t *c);                                             /* subscription bits of the link att_set_link named (gatts_subscribed does the link lookup) */
void gatt_link_reset(int k);                                                    /* a link went away: its subscriptions and MTU */
void att_set_link(int k);                                                       /* link the next att_ call works on */
void gatt_reset(void);                                                          /* new connection: subscriptions off */
int  gatts_subscribed(const gatt_chr_t *c);                                     /* bit 0 notifications, bit 1 indications */
int  att_indication_pending(void);
void att_indication_start(void);                                                /* gap.c: an indication has been sent */                                              /* an indication awaits its confirmation */

/* radio side (gap.c): */
int  gatts_init(const char *device_name);                                       /* GAP (0x1800: device name) + GATT (0x1801) services; call before your own */
void gatts_set_privacy(uint32_t rotate_ms);                                    /* after gatts_advertise: advertise with a resolvable private address, renewed every rotate_ms (0 = the static address again) */
int  gatts_advertise(const uint8_t addr[6], uint32_t interval_us);              /* connectable advertising of the device name; re-advertises by itself after a disconnect */
void gatt_poll(uint32_t wait_us);                                               /* main loop: sleep up to wait_us for a PDU, then drain the link layer: ATT, LL control, L2CAP signalling */
int  gatts_notify(const gatt_chr_t *c, const uint8_t *v, unsigned n);          /* 0 sent; < 0 not connected / not subscribed / n > MTU - 3 / link busy */
int  gatts_indicate(const gatt_chr_t *c, const uint8_t *v, unsigned n);        /* as notify, one in flight until the central confirms */

/* central role: initiate a connection to a connectable advertiser and wait for it (running gatt_poll); 0 connected, -1 busy, -2 not connected within timeout_ms */
int gattc_connect(const uint8_t peer[6], int peer_random, uint32_t timeout_ms);

/* ---- GATT client (gattc.c): the same connection, the other role. Calls block (running gatt_poll) until the answer or a 5 s timeout.
 * Return: >= 0 success (length / count), -1 not connected or timeout, -(0x100 + code) an ATT error from the server. */
typedef struct { uint16_t svc_start, svc_end, uuid16; } gattc_svc_t;                    /* uuid16 = 0 for a 128-bit service */
typedef struct { uint16_t decl, value, cccd; uint8_t props; uint16_t uuid16; } gattc_chr_t;   /* cccd = 0 when the characteristic has none */
int gattc_discover(gattc_svc_t *s, unsigned max, unsigned *n);                         /* all primary services */
int gattc_chars(const gattc_svc_t *s, gattc_chr_t *c, unsigned max, unsigned *n);      /* the service's characteristics, with CCCD handles */
int gattc_read(uint16_t handle, uint8_t *out, unsigned max);                            /* reads past one PDU with Read Blob */
int gattc_write(uint16_t handle, const uint8_t *v, unsigned n, int with_response);      /* n <= 20 */
int gattc_subscribe(const gattc_chr_t *c, int indicate, void (*cb)(uint16_t handle, const uint8_t *v, unsigned n));   /* writes the CCCD; cb runs inside gatt_poll */
void gattc_rx(const uint8_t *pdu, unsigned n);                                          /* gap.c: a client-bound ATT PDU arrived */
int  gatt_send_att(const uint8_t *att, unsigned n);                                     /* gap.c: one ATT PDU on the link */
