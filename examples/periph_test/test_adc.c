#include "test.h"
#include "adc.h"
#include "efuse.h"
#include "gpio.h"
#include "soc.h"
#include "sys.h"

/* ADC1 ch9 = GPIO10, wired to GPIO11 (jumper). GPIO11 sets the voltage. */
static void stats(const char *name, int *mean_mv)
{
    int32_t sum = 0, mn = 9999, mx = -1;
    for (int i = 0; i < 1000; i++) {
        int r = adc_read_raw(1, 9);
        sum += r; if (r < mn) mn = r; if (r > mx) mx = r;
    }
    int mean = sum / 1000;
    *mean_mv = adc_raw_to_mv(1, ADC_ATTEN_12DB, mean);
    printf("    %-22s raw mean %4d (min %4ld max %4ld)  = %4d mV\n", name, mean, mn, mx, *mean_mv);
}

void test_adc(void)
{
    uint8_t mac[6];
    efuse_mac(mac);
    printf("    eFuse MAC %02x:%02x:%02x:%02x:%02x:%02x, ADC calibration %s\n",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], adc_calibrated() ? "v1 present" : "MISSING");
    CHECK(mac[0] == 0x24 && mac[1] == 0x58 && mac[5] == 0xD0);         /* devkit 24:58:7C:E2:C9:D0 */
    CHECK(adc_calibrated());
    CHECK(adc_channel(1, 9, ADC_ATTEN_12DB) == 0);
    int lo, mid, hi;
    gpio_config(11, GPIO_OUTPUT); gpio_clr(11); delay_us(100);
    stats("GPIO11 low (0 V)", &lo);
    gpio_config(11, GPIO_INPUT | GPIO_PULLUP);                           /* pull-up vs pull-down */
    REG_SET(IO_MUX_GPIO(10), BIT(7));
    delay_us(1000);
    stats("pull-up/pull-down", &mid);
    REG_CLR(IO_MUX_GPIO(10), BIT(7));
    gpio_config(11, GPIO_OUTPUT); gpio_set(11); delay_us(100);
    stats("GPIO11 high (3.3 V)", &hi);
    CHECK(lo < 50 && mid > 1000 && mid < 2500 && hi > 2900);
    /* cross-check with ESP-IDF's curve fitting on this chip: raw 1859 -> 1490 mV, 4095 -> 3063 mV */
    int a = adc_raw_to_mv(1, ADC_ATTEN_12DB, 1859), b = adc_raw_to_mv(1, ADC_ATTEN_12DB, 4095);
    printf("    raw 1859 -> %d mV (IDF 1490), raw 4095 -> %d mV (IDF 3063)\n", a, b);
    CHECK(a == 1490 && b == 3063);
    METRIC("adc_read_raw()", "%lu cycles", CYCLES(adc_read_raw(1, 9)));
    gpio_config(11, GPIO_INPUT);
}
