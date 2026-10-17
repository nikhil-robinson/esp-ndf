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
void twai_recover(void);                                     /* leave bus-off */
