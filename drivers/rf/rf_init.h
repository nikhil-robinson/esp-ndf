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
extern uint32_t rf_gain_shadow[256][2];         /* [row] = { lo, hi } as last written */

/* RX gain rows (row = forced gain index): DC offset DACs, offset binary, DC rises with the code
 *   I stage 1 hi[30:22] (9 bits, before the VGA), I stage 2 hi[21:14] (8 bits)
 *   Q stage 1 lo[16:8],                           Q stage 2 lo[7:0]
 * rf_rx_dc_cal(): our calibration with the dump engine, rows 0..rows-1, one solve per group of
 * rows sharing gain codes; receive must be on and tuned. Returns the worst residual |DC| x16. */
int rf_rx_dc_cal(unsigned rows);
void rf_init_replay(void);                      /* this board's vendor init, as data */
void rf_rx_on(void);                            /* receive front end and RX clock on */
#ifndef RF_LO_DIRECT_MIN_KHZ
#define RF_LO_DIRECT_MIN_KHZ 2240000u          /* below: receive through the 6/5 LO divider */
#endif
int  rf_tune_rx_khz(uint32_t khz);              /* receive LO 1.85..2.76 GHz; 0, or -3 if out of the VCO window */
void rf_rx_gain(int gain);                      /* <0: AGC; else forced index (0..127) */

/* transmit */
void rf_tone_off(void);                         /* tone generator off: DAC data path */
void rf_pbus_xpd_tx_on(uint16_t pa, uint16_t mix);
void rf_tx_gain(uint8_t gain, uint16_t bb, int8_t dig);   /* fills the Wi-Fi and BT TX gain rows */
void rf_txon(int on);                           /* force the transmitter on without a frame */
void rf_tx_on(uint8_t gain, uint16_t bb);       /* tone off, TX front end on, gain, forced on */
void rf_tx_off(void);
void rf_tx_dc_cal(uint8_t gain, uint16_t bb);   /* our TX LO-leakage calibration (power detector); after rf_tx_on */
void rf_tx_dc_reset(void);                      /* neutral TX DC words */
void rf_tx_set_iq(uint16_t v);                  /* TX IQ word: gain [4:0], phase [10:5], signed; default 0 */
void rf_loopback(int on, uint16_t pa, uint16_t rxg);   /* internal TX -> RX path; pa 0xFFFF: switch and clocks only */
void rf_pwdet_on(void);                         /* TX power detector via SAR2 */
uint32_t rf_pwdet_read(void);                   /* sum of its 8 latest 13-bit results */
void rf_bb_est(uint16_t n, int32_t *i, int32_t *q, int32_t *pwr);   /* hardware DC / power estimate over n+1 samples */
