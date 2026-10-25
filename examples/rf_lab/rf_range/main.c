/* PLL tuning range (700 MHz .. 3.4 GHz) and the RC calibration code read back after init. */
#include "console.h"
#include "sys.h"
#include <stdint.h>
#include "rf_regi2c.h"
#include "rf_pll.h"
#include "rf_init.h"

int main(void)
{
    delay_ms(1500);
    rf_init_replay();
    printf("=== rf_range\n");
    printf("after init: 67.14 %02x 67.1c %02x 67.16 %02x (C9:48 trace: 14 1d 0f, D0 trace: 15 1e 10)\n", rf_regi2c_read(0x67, 0x14), rf_regi2c_read(0x67, 0x1C), rf_regi2c_read(0x67, 0x16));
    for (uint32_t mhz = 700; mhz <= 3400; mhz += 100) {
        rf_pll_result_t r = { 0 };
        int rc = rf_pll_set_khz(mhz * 1000, &r);
        printf("%4lu MHz rc %d cap %03x hw %02x in-window %d\n", (unsigned long)mhz, rc, r.cap, r.hw_cap, r.locked);
    }
    printf("=== done\n");
    for (;;) {}
}
