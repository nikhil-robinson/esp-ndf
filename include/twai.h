/* TWAI (CAN 2.0A/B) controller, SJA1000-compatible. Needs an external
 * transceiver on a real bus; TWAI_SELFTEST loops back without one. */
#pragma once
#include "ndf.h"

typedef struct {
    uint32_t id;           /* 11-bit or 29-bit */
    bool     ext, rtr;
    uint8_t  dlc;          /* 0..8 */
    uint8_t  data[8];
} twai_frame_t;

typedef enum { TWAI_NORMAL, TWAI_SELFTEST, TWAI_LISTEN } twai_mode_t;

/* bitrate: 25k..1M; sample point ~80 %. Accepts all IDs until twai_filter(). */
int  twai_init(int tx, int rx, uint32_t bitrate, twai_mode_t mode);
/* Single acceptance filter: frame accepted if (id & mask) == (code & mask). */
void twai_filter(uint32_t code, uint32_t mask, bool ext);
int  twai_send(const twai_frame_t *f, uint32_t timeout_us);
int  twai_recv(twai_frame_t *f, uint32_t timeout_us);      /* 0 ok, -1 timeout */
void twai_counters(uint8_t *tx_err, uint8_t *rx_err, uint32_t *rx_dropped);
void twai_recover(void);        /* from bus-off: start recovery (128 x 11 recessive bits), then twai_start() */
void twai_start(void);          /* back on the bus after recovery */

/* Alerts (as IDF's TWAI_ALERT_*), from the interrupt; twai_alerts() returns and clears the ones so far. */
enum { TWAI_ALERT_TX_DONE = 1, TWAI_ALERT_RX_OVERRUN = 2, TWAI_ALERT_ARB_LOST = 4, TWAI_ALERT_BUS_ERROR = 8,
       TWAI_ALERT_ABOVE_ERR_WARN = 16, TWAI_ALERT_BELOW_ERR_WARN = 32, TWAI_ALERT_ERR_PASSIVE = 64,
       TWAI_ALERT_ERR_ACTIVE = 128, TWAI_ALERT_BUS_OFF = 256, TWAI_ALERT_RECOVERED = 512 };
enum { TWAI_ST_ACTIVE, TWAI_ST_PASSIVE, TWAI_ST_BUS_OFF, TWAI_ST_RECOVERING };
uint32_t twai_alerts(void);
int      twai_state(void);
void     twai_on_alert(void (*fn)(uint32_t alerts, void *arg), void *arg);
