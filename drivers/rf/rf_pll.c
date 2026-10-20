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
#include "ndf.h"

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
static uint8_t vol(void) { return rf_regi2c_read(0x62, 0x0c) & 0x0C; }   /* bits 3:2: 0 in window, 4 low, 8 high (bit 1 can be set: D0) */

static int set_n(uint64_t n, rf_pll_result_t *res);
static uint8_t cache[5];                         /* last written 63.03, 63.04, 63.05, 62.01, 62.02 (hop skips unchanged bytes) */
static int cache_ok;
unsigned rf_pll_hop_writes;                      /* debug: bit k = byte k written by the last hop */
static void cache_load(void)
{
    cache[0] = rf_regi2c_read(0x63, 0x03); cache[1] = rf_regi2c_read(0x63, 0x04); cache[2] = rf_regi2c_read(0x63, 0x05);
    cache[3] = rf_regi2c_read(0x62, 0x01); cache[4] = rf_regi2c_read(0x62, 0x02); cache_ok = 1;
}

int rf_pll_set_khz(uint32_t khz, rf_pll_result_t *res)
{
    /* N * 2^16 = (4 f / 120 MHz - 32) * 2^16 = (f_khz * 4 * 65536) / 120000 - 32 * 65536 */
    return set_n((uint64_t)khz * 4u * 65536u / 120000u - 32u * 65536u, res);   /* truncated, as the vendor does */
}

/* Hz resolution: one LSB of the 16-bit fraction is 30 MHz / 65536 = 457.76 Hz (direct path). Rounded. */
RAM_FN static uint64_t n_of_hz(uint64_t hz) { return (hz * 4u * 65536u + 60000000u) / 120000000u - 32u * 65536u; }
int rf_pll_set_hz(uint64_t hz, rf_pll_result_t *res) { return set_n(n_of_hz(hz), res); }

static int set_n(uint64_t n, rf_pll_result_t *res)
{
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
    cache_load();
    return 0;
}

/* Fast retune for hopping: divider and a cap known from an earlier
 * rf_pll_set_khz at that frequency (res.cap); no calibration, no search.
 * The calibration mux (62.0b[6]) is left as rf_pll_set_khz leaves it. */
RAM_FN static void hop_n(uint32_t n, uint16_t cap);
uint32_t rf_pll_n_of_hz(uint64_t hz) { return (uint32_t)n_of_hz(hz); }
uint16_t rf_pll_cap(void) { if (!cache_ok) cache_load(); return (uint16_t)(cache[3] | ((cache[4] >> 4) & 1) << 8); }
RAM_FN void rf_pll_hop_n(uint32_t n, uint16_t cap) { hop_n(n, cap); }
RAM_FN void rf_pll_hop(uint32_t khz, uint16_t cap) { hop_n((uint32_t)((uint64_t)khz * 4u * 65536u / 120000u - 32u * 65536u), cap); }
RAM_FN void rf_pll_hop_hz(uint64_t hz, uint16_t cap) { hop_n((uint32_t)n_of_hz(hz), cap); }
/* Measured: the 63.00 latch writes are not needed, and only changed bytes need writing. A hop within the band
 * typically writes the two fraction bytes and the cap low byte: 3 bus transactions = 10.3 us (3.44 us each,
 * the analog bus floor; its clock is not in 0x6000E050..E060). Integer N and cap bit 8 add 3.44 us when they change. */
RAM_FN static void hop_n(uint32_t n, uint16_t cap)
{
    if (!cache_ok) cache_load();
    uint8_t b[5] = { (uint8_t)(n >> 16), (uint8_t)(n >> 8), (uint8_t)n, (uint8_t)cap, (uint8_t)((cache[4] & ~0x10u) | ((cap >> 8) & 1) << 4) };
    static const uint8_t blk[5] = { 0x63, 0x63, 0x63, 0x62, 0x62 }, reg[5] = { 0x03, 0x04, 0x05, 0x01, 0x02 };
    rf_pll_hop_writes = 0;
    for (int k = 0; k < 5; k++) if (b[k] != cache[k]) { rf_regi2c_write(blk[k], reg[k], b[k]); cache[k] = b[k]; rf_pll_hop_writes |= 1u << k; }
}

/* RF frequency memory: the per-MHz table the BLE hardware tunes the LO from on its own (each event,
 * each hop). Write-only, behind 0x6000E0C4[7:0] (address) / 0x6000E148 (data) / 0x6000E0C4 bit 9 (write).
 * 85 entries, 2400..2484 MHz, 3 words each, as libphy get_rf_freq_init builds them:
 *   w0 = cap[7:0] | (0x80 | vco code) << 8 | cap[8] << 12, cap interpolated from calibrations at 2400 and 2464 MHz
 *   w1 = divider N (int, frac hi, frac lo), w2 = 0.
 * Without it the BLE core runs its events but the LO never lands on the channel. */
#define FM_CTRL 0x6000E0C4u
#define FM_DATA 0x6000E148u
static void fm_write(unsigned addr, uint32_t v)
{
    volatile uint32_t *c = (volatile uint32_t *)FM_CTRL;
    *c = (*c & ~0xFFu) | (addr & 0xFF);
    *(volatile uint32_t *)FM_DATA = v;
    *c |= 1u << 9;
    *c &= ~(1u << 9);
}

void rf_freq_mem_load(void)
{
    rf_pll_result_t r;
    rf_regi2c_write_bits(0x62, 0x02, 7, 7, 0);
    rf_pll_set_khz(2437000, &r);
    uint8_t vco = rf_regi2c_read_bits(0x62, 0x06, 3, 0);
    rf_regi2c_write_bits(0x62, 0x02, 3, 0, vco);
    rf_regi2c_write_bits(0x62, 0x02, 7, 7, 1);
    rf_pll_set_khz(2400000, &r); int cap0 = r.cap;
    rf_pll_set_khz(2464000, &r); int step = (int)r.cap - cap0;   /* per 64 MHz */
    int acc = 0;
    for (unsigned k = 0; k < 85; k++, acc += step) {
        uint32_t n = (uint32_t)((uint64_t)(2400000u + 1000u * k) * 4u * 65536u / 120000u - 32u * 65536u);
        int cap = cap0 + acc / 64;
        if (cap < 0) cap = 0;
        if (cap > 0x1FF) cap = 0x1FF;
        fm_write(3 * k + 0, (uint32_t)(cap & 0xFF) | ((uint32_t)(((cap >> 8) << 4 | 0x80 | vco) & 0xFF) << 8));
        fm_write(3 * k + 1, n & 0xFFFFFF);
        fm_write(3 * k + 2, 0);
    }
}
