/* TX LO-leakage measurement with our own power detector (no second board):
 * transmit with no DAC data, sweep a TX-row DC field, read SAR2. */
#include "console.h"
#include "sys.h"
#include <stdint.h>
#include "../../drivers/rf/rf_pll.h"
#include "../../drivers/rf/rf_init.h"
#include "../../drivers/rf/rf_dump.h"

#define FE0 (*(volatile uint32_t *)0x60006000)

int main(void)
{
    delay_ms(1500);
    printf("=== rf_txcal probe\n");
    rf_init_replay();
    rf_pll_result_t r;
    rf_pll_set_khz(2437000, &r);
    rf_rx_on();
    rf_tx_on(127, 0);
    rf_pwdet_on();
    unsigned base = FE0 >> 18 & 0xFF;
    uint32_t lo0 = rf_gain_shadow[base][0], hi0 = rf_gain_shadow[base][1];
    for (int loop = 0; loop < 2; loop++) {
        rf_loopback(loop, 15, 0x80);
        if (loop) rf_txon(1);
        printf("loopback %d, w0 sweep (lo[20:12]), base value %03lx\n", loop, (unsigned long)(lo0 >> 12 & 0x1FF));
        for (int v = 0x60; v <= 0x1A0; v += 0x10) {
            uint32_t lo = (lo0 & ~(0x1FFu << 12)) | (uint32_t)v << 12;
            for (unsigned k = 0; k < 14; k++) rf_gain_mem_write(base + k, lo, hi0);
            rf_txon(0); rf_txon(1); delay_us(200);
            uint32_t a = 0; for (int k = 0; k < 16; k++) { a += rf_pwdet_read(); delay_us(20); }
            printf("  w0 %03x  sar %lu  e080 %08lx\n", v, (unsigned long)a, (unsigned long)*(volatile uint32_t *)0x6000E080);
        }
    }
    printf("=== done\n");
    for (;;) {}
}
