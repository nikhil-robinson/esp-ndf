/* Our RF code vs the vendor PHY on the same chip. Step 1: PLL tuning.
 * The vendor init brings the chip up (to be replaced later); then for each
 * frequency: vendor set_rf_freq_offset -> read N and cap; ours -> read N and
 * cap; compare. */
#include <stdio.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_phy_init.h"
#include "esp_cpu.h"
#include "rf_regi2c.h"
#include "rf_pll.h"
void esp_wifi_bt_power_domain_on(void);
#include "esp_rom_sys.h"
void rf_delay_us(uint32_t us) { esp_rom_delay_us(us); }   /* platform hook */
extern void set_chanfreq(uint32_t mhz, uint32_t mode);
extern void set_rf_freq_offset(uint32_t xtal_sel, uint32_t mhz, int32_t khz);

static void snap(uint8_t *n, uint16_t *cap)
{
    n[0] = rf_regi2c_read(0x63, 3); n[1] = rf_regi2c_read(0x63, 4); n[2] = rf_regi2c_read(0x63, 5);
    *cap = (uint16_t)(rf_regi2c_read_bits(0x62, 2, 4, 4) << 8 | rf_regi2c_read(0x62, 1));
}

void app_main(void)
{
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_wifi_bt_power_domain_on();
    esp_phy_enable(PHY_MODEM_WIFI);
    *(volatile uint32_t *)0x60026014 |= 0x00400000;
    int ok = 0, total = 0;
    for (uint32_t step = 0; step < 63 + 6; step++) {
        uint32_t mhz = step < 63 ? 1850 + 15 * step : 2360;
        int32_t khz = step < 63 ? (int32_t)((mhz * 37) % 1000) : 320;     /* odd kHz offsets; then the 0x100 case 6 times */
        uint8_t nv[3], no[3]; uint16_t cv, co;
        set_chanfreq(2437, 0); set_rf_freq_offset(0, mhz, khz); snap(nv, &cv);
        rf_pll_result_t r = { 0 };
        uint32_t t0 = esp_cpu_get_cycle_count();
        int rc = rf_pll_set_khz(mhz * 1000 + khz, &r);
        uint32_t cyc = esp_cpu_get_cycle_count() - t0;
        snap(no, &co);
        int same = nv[0] == no[0] && nv[1] == no[1] && nv[2] == no[2] && cv == co;
        total++; ok += same;
        printf("%4lu.%03ld MHz vendor N=%02x.%02x%02x cap=%03x | ours N=%02x.%02x%02x cap=%03x hw=%02x win=%d [%03x..%03x] rc=%d %luus %s\n",
               (unsigned long)mhz, (long)khz, nv[0], nv[1], nv[2], cv, no[0], no[1], no[2], co, r.hw_cap, r.locked, r.lo, r.hi, rc,
               (unsigned long)(cyc / 240), same ? "SAME" : "DIFF");
    }
    printf("=== %d/%d identical\n", ok, total);
}
