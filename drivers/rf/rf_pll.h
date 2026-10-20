/* RF synthesizer (LO). */
#pragma once
#include <stdint.h>

typedef struct {
    uint16_t cap;         /* VCO capacitor chosen (9 bits) */
    uint8_t  hw_cap;      /* the hardware calibration's pick (low 8 bits) */
    uint8_t  locked;      /* control voltage in window at the chosen cap (62.0C == 0) */
    uint32_t cal_polls;   /* polls of the calibration-done bit */
    uint16_t lo, hi;      /* in-window cap range found by the refinement */
} rf_pll_result_t;

/* Tune the LO to f (kHz) with a 40 MHz crystal: N = 4 f / (3 * 40 MHz) - 32,
 * 8.16 fixed point, then the hardware VCO-cap calibration. */
int rf_pll_set_khz(uint32_t khz, rf_pll_result_t *res);
/* Fast retune (hopping): divider + a cap measured before by rf_pll_set_khz. */
void rf_pll_hop(uint32_t khz, uint16_t cap);
/* Hz resolution (step 30 MHz / 65536 = 457.76 Hz on the direct path, rounded) */
int  rf_pll_set_hz(uint64_t hz, rf_pll_result_t *res);
void rf_pll_hop_hz(uint64_t hz, uint16_t cap);
/* fastest hop: N precomputed (the 64-bit division costs ~16 us on this core), only changed bytes written:
 * 3 bus transactions within a band = ~10 us from IRAM */
uint32_t rf_pll_n_of_hz(uint64_t hz);
void rf_pll_hop_n(uint32_t n, uint16_t cap);
uint16_t rf_pll_cap(void);                  /* VCO cap now in use */
/* Load the RF frequency memory (per-MHz LO table, 2400..2484) the BLE hardware tunes from; after rf_init_replay. */
void rf_freq_mem_load(void);
