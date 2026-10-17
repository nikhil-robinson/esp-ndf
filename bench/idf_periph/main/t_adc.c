#include "t.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali_scheme.h"
#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "soc/io_mux_reg.h"
#include "esp_rom_sys.h"
static adc_oneshot_unit_handle_t u;
static adc_cali_handle_t cal;
static void stats(const char *name)
{
    int sum = 0, mn = 9999, mx = -1, r;
    for (int i = 0; i < 1000; i++) { adc_oneshot_read(u, ADC_CHANNEL_9, &r); sum += r; if (r < mn) mn = r; if (r > mx) mx = r; }
    int mean = sum / 1000, mv = -1;
    adc_cali_raw_to_voltage(cal, mean, &mv);
    printf("    %-22s raw mean %4d (min %4d max %4d)  = %4d mV\n", name, mean, mn, mx, mv);
}
void t_adc(void)
{
    adc_oneshot_unit_init_cfg_t uc = { .unit_id = ADC_UNIT_1 };
    adc_oneshot_new_unit(&uc, &u);
    adc_oneshot_chan_cfg_t cc = { .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_12 };
    adc_oneshot_config_channel(u, ADC_CHANNEL_9, &cc);
    adc_cali_curve_fitting_config_t cf = { .unit_id = ADC_UNIT_1, .chan = ADC_CHANNEL_9, .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_12 };
    printf("    calibration: %s\n", adc_cali_create_scheme_curve_fitting(&cf, &cal) == ESP_OK ? "curve fitting" : "FAILED");
    gpio_reset_pin(11);
    gpio_set_direction(11, GPIO_MODE_OUTPUT); gpio_set_level(11, 0); esp_rom_delay_us(100);
    stats("GPIO11 low (0 V)");
    gpio_set_direction(11, GPIO_MODE_INPUT); gpio_set_pull_mode(11, GPIO_PULLUP_ONLY);
    rtc_gpio_pulldown_en(10);                       /* IDF puts ADC pads on the RTC mux */
    esp_rom_delay_us(1000);
    stats("pull-up/pull-down");
    rtc_gpio_pulldown_dis(10);
    gpio_set_direction(11, GPIO_MODE_OUTPUT); gpio_set_level(11, 1); esp_rom_delay_us(100);
    stats("GPIO11 high (3.3 V)");
    int r;
    METRIC("adc_read_raw()", "%lu cycles", CYCLES(adc_oneshot_read(u, ADC_CHANNEL_9, &r)));
    gpio_reset_pin(11);
}
