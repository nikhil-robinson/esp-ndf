/* RF PLL programming, as traced from the vendor sequence (bench/idf_rftrace):
 *   62.0b[6] = 0                       calibration mux off
 *   63.00 = 0x07, 63.03 = N int, 63.04/05 = N fraction, 63.00 = 0x17 (latch)
 *   62.00[6] = 1, [5] = 0, [5] = 1, [6] = 0     start VCO-cap calibration
 *   poll 62.07[1] (done); 62.05 = chosen cap; 62.07[2] = lock
 *   62.0b[6] = 1
 *   62.01 = cap[7:0], 62.02[4] = cap[8]
 * Refinement (traced at 1900/2437/2700 MHz, rule fits all): 62.0C reads 4
 * (control voltage low), 8 (high) or 0 (in window). Walk the cap down from
 * the hardware pick (at most 10 steps) and up (at most 10) while the reading
 * is 0; take the middle of the in-window range. If the hardware pick itself
 * is out of the window, keep it. */
#include "rf_pll.h"
#include "rf_regi2c.h"

/* settle time after a cap change before the voltage comparator is read
 * (measured: reading straight away misreads the step across bit 8) */
#ifndef RF_PLL_SETTLE_US
#define RF_PLL_SETTLE_US 5
#endif
extern void rf_delay_us(uint32_t us);

static void write_cap(uint16_t c)
{
    rf_regi2c_write(0x62, 0x01, (uint8_t)c);
    rf_regi2c_write_bits(0x62, 0x02, 4, 4, (uint8_t)(c >> 8));
    rf_delay_us(RF_PLL_SETTLE_US);
}
static uint8_t vol(void) { return rf_regi2c_read(0x62, 0x0c); }   /* 0 in window, 4 low, 8 high */

int rf_pll_set_khz(uint32_t khz, rf_pll_result_t *res)
{
    /* N * 2^16 = (4 f / 120 MHz - 32) * 2^16 = (f_khz * 4 * 65536) / 120000 - 32 * 65536 */
    uint64_t n = (uint64_t)khz * 4u * 65536u / 120000u - 32u * 65536u;   /* truncated, as the vendor does */
    if (n >> 24) return -1;
    rf_regi2c_write_bits(0x62, 0x0b, 6, 6, 0);
    rf_regi2c_write(0x63, 0x00, 0x07);
    rf_regi2c_write(0x63, 0x03, (uint8_t)(n >> 16));
    rf_regi2c_write(0x63, 0x04, (uint8_t)(n >> 8));
    rf_regi2c_write(0x63, 0x05, (uint8_t)n);
    rf_regi2c_write(0x63, 0x00, 0x17);
    rf_regi2c_write_bits(0x62, 0x00, 6, 6, 1);
    rf_regi2c_write_bits(0x62, 0x00, 5, 5, 0);
    rf_regi2c_write_bits(0x62, 0x00, 5, 5, 1);
    rf_regi2c_write_bits(0x62, 0x00, 6, 6, 0);
    uint32_t polls = 0;
    while (!rf_regi2c_read_bits(0x62, 0x07, 1, 1)) if (++polls > 100000) return -2;
    uint16_t hw = (uint16_t)(rf_regi2c_read_bits(0x62, 0x07, 2, 2) << 8 | rf_regi2c_read(0x62, 0x05));   /* 9-bit cap */
    rf_regi2c_write_bits(0x62, 0x0b, 6, 6, 1);
    uint16_t c = hw;
    write_cap(hw);
    if (vol() == 0) {
        int lo = hw, hi = hw;
        for (int i = 1; i <= 10 && hw - i >= 0; i++) { write_cap((uint16_t)(hw - i)); if (vol()) break; lo = hw - i; }
        for (int i = 1; i <= 10 && hw + i <= 0x1FF; i++) { write_cap((uint16_t)(hw + i)); if (vol()) break; hi = hw + i; }
        c = (uint16_t)((lo + hi) / 2);
        if (res) { res->lo = (uint16_t)lo; res->hi = (uint16_t)hi; }
    }
    write_cap(c);
    if (res) { res->cap = c; res->hw_cap = (uint8_t)hw; res->locked = vol() == 0; res->cal_polls = polls; }
    return 0;
}

/* Fast retune for hopping: divider and a cap known from an earlier
 * rf_pll_set_khz at that frequency (res.cap); no calibration, no search.
 * The calibration mux (62.0b[6]) is left as rf_pll_set_khz leaves it. */
void rf_pll_hop(uint32_t khz, uint16_t cap)
{
    uint32_t n = (uint32_t)((uint64_t)khz * 4u * 65536u / 120000u - 32u * 65536u);
    rf_regi2c_write(0x63, 0x00, 0x07);
    rf_regi2c_write(0x63, 0x03, (uint8_t)(n >> 16));
    rf_regi2c_write(0x63, 0x04, (uint8_t)(n >> 8));
    rf_regi2c_write(0x63, 0x05, (uint8_t)n);
    rf_regi2c_write(0x63, 0x00, 0x17);
    rf_regi2c_write(0x62, 0x01, (uint8_t)cap);
    rf_regi2c_write_bits(0x62, 0x02, 4, 4, (uint8_t)(cap >> 8));
}
