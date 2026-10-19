/* PLL tuning range: our rf_pll_set_khz from 700 MHz to 3.4 GHz. */
#include "console.h"
#include "sys.h"
#include <stdint.h>
#include "../../drivers/rf/rf_pll.h"
#include "../../drivers/rf/rf_init.h"

int main(void)
{
    delay_ms(1500);
    rf_init_replay();
    printf("=== rf_range\n");
    for (uint32_t mhz = 700; mhz <= 3400; mhz += 100) {
        rf_pll_result_t r = { 0 };
        int rc = rf_pll_set_khz(mhz * 1000, &r);
        printf("%4lu MHz rc %d cap %03x hw %02x in-window %d\n", (unsigned long)mhz, rc, r.cap, r.hw_cap, r.locked);
    }
    printf("=== done\n");
    for (;;) {}
}
