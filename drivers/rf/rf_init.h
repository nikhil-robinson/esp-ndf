/* ESP32-S3 RF bring-up and receive path without the vendor PHY: power
 * domain, pbus (front-end control bus), gain memory, init replay, forced
 * receive gain. Register behaviour from the ROM disassembly and traces
 * (bench/idf_rftrace, docs/PHY_STANDALONE.md). */
#pragma once
#include <stdint.h>

typedef struct { uint32_t addr, value; } rf_reg_t;
/* op 1 analog (a block, b reg, c msb<<4|lsb, x data); 2 pbus (a row, b col,
 * x value); 3 gain memory (a index, x low word, y high word); 4 pbus force mode (a on) */
typedef struct { uint8_t op, a, b, c; uint32_t x, y; } rf_op_t;

void rf_power_on(void);                         /* modem power domain + resets */
void rf_pbus_write(uint8_t row, uint8_t col, uint16_t value);
void rf_pbus_force_mode(int on);               /* off: hardware owns the front end */
void rf_pbus_xpd_tx_off(void);
void rf_pbus_xpd_rx_on(uint16_t rxgain);
void rf_gain_mem_write(uint8_t index, uint32_t lo, uint32_t hi);
void rf_init_replay(void);                      /* this board's vendor init, as data */
void rf_rx_on(void);                            /* receive front end and RX clock on */
void rf_rx_gain(int gain);                      /* <0: AGC; else forced index (0..127) */
