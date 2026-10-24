/* IDF sibling of examples/rtc_hold: GPIO4 high + RTC hold through esp_restart, released 300 ms after reboot. */
#include <stdio.h>
#include "driver/rtc_io.h"
#include "esp_system.h"
#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static RTC_NOINIT_ATTR uint32_t magic;

void app_main(void)
{
    if (magic != 0x484F4C44) {
        rtc_gpio_init(4); rtc_gpio_set_direction(4, RTC_GPIO_MODE_OUTPUT_ONLY); rtc_gpio_set_level(4, 1);
        rtc_gpio_hold_en(4);
        magic = 0x484F4C44;
        printf("idf_rtc_hold: GPIO4 high + held, restart in 200 ms\n");
        vTaskDelay(pdMS_TO_TICKS(200));
        esp_restart();
    }
    magic = 0;
    printf("idf_rtc_hold: rebooted, releasing in 300 ms\n");
    vTaskDelay(pdMS_TO_TICKS(300));
    rtc_gpio_set_level(4, 0); rtc_gpio_hold_dis(4);
    printf("idf_rtc_hold done\n");
}
