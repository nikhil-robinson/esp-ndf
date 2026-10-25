/* RX DC offset calibration with our own capture (no vendor estimator). DC is
 * linear in each offset DAC code. Stage 1 (before the VGA) depends only on
 * the stages above the VGA, so it is solved once per group of rows sharing
 * lo[31:22] and hi[13:0] (19 groups): two measurements give the slope, a
 * third refines. Stage 2 (after the VGA) has one slope for all rows: one
 * measurement per row. */
#include "rf_init.h"
#include "rf_dump.h"
#include <stdint.h>

void rf_delay_us(uint32_t us);

#define NS     512u                             /* samples per measurement, 4 blocks */
#define BLK    128u
#define SETTLE 10u                              /* us after a gain change */

/* DC x16 of the quietest block (a burst on the air must not bias it) */
static void measure(int row, int *di, int *dq)
{
    rf_rx_gain((row + 64) & 127);               /* a row write takes effect when the gain index changes */
    rf_rx_gain(row);
    rf_delay_us(SETTLE);
    rf_dump_burst(RF_DUMP_80M, NS);
    const uint32_t *w = (const uint32_t *)RF_DUMP_BURST_ADDR;
    int64_t best = INT64_MAX;
    for (uint32_t b = 0; b < NS; b += BLK) {
        int32_t si = 0, sq = 0, pi = 0, pq = 0;
        for (uint32_t k = b; k < b + BLK; k++) { int i = rf_iq_i(w[k]), q = rf_iq_q(w[k]); si += i; sq += q; pi += i * i; pq += q * q; }
        int64_t var = (int64_t)pi * BLK - (int64_t)si * si + (int64_t)pq * BLK - (int64_t)sq * sq;
        if (var < best) { best = var; *di = si / (int)(BLK / 16); *dq = sq / (int)(BLK / 16); }
    }
}

static uint32_t get(int row, int word, int sh, uint32_t mask) { return rf_gain_shadow[row][word] >> sh & mask; }
static void put(int row, int word, int sh, uint32_t mask, uint32_t v)
{
    rf_gain_shadow[row][word] = (rf_gain_shadow[row][word] & ~(mask << sh)) | (v & mask) << sh;
}
static void commit(int row) { rf_gain_mem_write((uint8_t)row, rf_gain_shadow[row][0], rf_gain_shadow[row][1]); }
static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
static int rdiv(int a, int b) { return (a + (a >= 0 ? b / 2 : -b / 2)) / b; }   /* rounded, b > 0 */

/* successive approximation on the sign of DC, I and Q together: robust to
 * saturation and to a wrong slope. Ends with the codes nearest zero DC. */
static void sar(int row, int ish, int qsh, uint32_t mask, int bits)
{
    int fi = 0, fq = 0, di, dq;
    for (int b = bits - 1; b >= 0; b--) {
        put(row, 1, ish, mask, fi | 1 << b); put(row, 0, qsh, mask, fq | 1 << b); commit(row);
        measure(row, &di, &dq);
        if (di <= 0) fi |= 1 << b;              /* DC rises with the code */
        if (dq <= 0) fq |= 1 << b;
    }
    put(row, 1, ish, mask, fi); put(row, 0, qsh, mask, fq); commit(row);
}

static void stage1(int row) { sar(row, 22, 8, 0x1FF, 9); }   /* I hi[30:22], Q lo[16:8] */

static int same_group(int a, int b)
{
    return (rf_gain_shadow[a][0] >> 22) == (rf_gain_shadow[b][0] >> 22) && (rf_gain_shadow[a][1] & 0x3FE7) == (rf_gain_shadow[b][1] & 0x3FE7);
}

int rf_rx_dc_cal(unsigned rows)
{
    int worst = 0;
    for (unsigned r = 0; r < rows; r++) { put(r, 1, 14, 0xFF, 128); put(r, 0, 0, 0xFF, 128); }
    for (int r = (int)rows - 1; r >= 0; r--) {  /* stage 1, once per group, on its highest VGA row (largest slope) */
        unsigned p = rows - 1;
        while ((int)p > r && !same_group(p, r)) p--;
        if ((int)p > r) { put(r, 1, 22, 0x1FF, get(p, 1, 22, 0x1FF)); put(r, 0, 8, 0x1FF, get(p, 0, 8, 0x1FF)); commit(r); continue; }
        stage1(r);
    }
    int i0, q0, i1, q1;                         /* stage 2 slope, from row 0 */
    measure(0, &i0, &q0);
    put(0, 1, 14, 0xFF, 160); put(0, 0, 0, 0xFF, 160); commit(0);
    measure(0, &i1, &q1);
    int si = i1 - i0, sq = q1 - q0;
    put(0, 1, 14, 0xFF, 128); put(0, 0, 0, 0xFF, 128); commit(0);
    if (si <= 0 || sq <= 0) return -1;
    for (unsigned r = 0; r < rows; r++) {       /* stage 2: one-shot with the common slope, verified */
        int di, dq;
        measure(r, &di, &dq);
        put(r, 1, 14, 0xFF, clampi(128 - rdiv(di * 32, si), 0, 255));
        put(r, 0, 0, 0xFF, clampi(128 - rdiv(dq * 32, sq), 0, 255));
        commit(r);
        measure(r, &di, &dq);
        if (di < -48 || di > 48 || dq < -48 || dq > 48) {   /* > 3 LSB: search instead */
            sar(r, 14, 0, 0xFF, 8);
            measure(r, &di, &dq);
        }
        if (di < 0) di = -di;
        if (dq < 0) dq = -dq;
        if (di > worst) worst = di;
        if (dq > worst) worst = dq;
    }
    return worst;
}
