#include "t.h"
#include "driver/rtc_io.h"
#include "driver/gpio.h"
#include "esp_cpu.h"
#include "esp_rom_sys.h"
void t_rtcio(void)
{
    rtc_gpio_init(10); rtc_gpio_set_direction(10, RTC_GPIO_MODE_OUTPUT_ONLY);
    rtc_gpio_init(11); rtc_gpio_set_direction(11, RTC_GPIO_MODE_INPUT_ONLY); rtc_gpio_pulldown_en(11);
    int ok = 0;
    for (int i = 0; i < 100; i++) { rtc_gpio_set_level(10, i & 1); esp_rom_delay_us(1); ok += rtc_gpio_get_level(11) == (i & 1); }
    uint32_t t = esp_cpu_get_cycle_count(); rtc_gpio_set_level(10, 1); uint32_t w = esp_cpu_get_cycle_count() - t;
    t = esp_cpu_get_cycle_count(); volatile int l = rtc_gpio_get_level(11); uint32_t r = esp_cpu_get_cycle_count() - t; (void)l;
    rtc_gpio_hold_en(10); rtc_gpio_set_level(10, 0); esp_rom_delay_us(5); int h = rtc_gpio_get_level(11);
    rtc_gpio_hold_dis(10); esp_rom_delay_us(5); int rel = rtc_gpio_get_level(11);
    printf("    loopback %d/100, held %d, released %d\n", ok, h, rel);
    printf("    rtc_gpio_set_level()             %lu cycles\n", w);
    printf("    rtc_gpio_get_level()             %lu cycles\n", r);
    rtc_gpio_deinit(10); rtc_gpio_deinit(11); gpio_reset_pin(10); gpio_reset_pin(11);
}
