/* IDF sibling of components/drivers/*/test_apps/*_unit test_twai_alerts: TX 10 -> jumper -> RX 11, 500 kbit/s, NORMAL mode. */
#include <stdio.h>
#include "driver/twai.h"
#include "esp_rom_gpio.h"
#include "esp_cpu.h"
#include "freertos/FreeRTOS.h"

void app_main(void)
{
    twai_general_config_t g = TWAI_GENERAL_CONFIG_DEFAULT(10, 11, TWAI_MODE_NORMAL);
    g.alerts_enabled = TWAI_ALERT_ALL;
    twai_timing_config_t t = TWAI_TIMING_CONFIG_500KBITS();
    twai_filter_config_t f = TWAI_FILTER_CONFIG_ACCEPT_ALL();
    twai_driver_install(&g, &t, &f); twai_start();
    twai_message_t m = { .identifier = 0x321, .data_length_code = 2 };
    esp_rom_gpio_connect_in_signal(0x38, 116, false);          /* RX stuck recessive -> bit errors -> bus-off */
    twai_transmit(&m, pdMS_TO_TICKS(10));
    uint32_t a = 0, all = 0;
    for (int i = 0; i < 20 && !(all & TWAI_ALERT_BUS_OFF); i++) if (twai_read_alerts(&a, pdMS_TO_TICKS(10)) == ESP_OK) all |= a;
    printf("bus-off alert: %s\n", all & TWAI_ALERT_BUS_OFF ? "yes" : "NO");
    esp_rom_gpio_connect_in_signal(11, 116, false);
    uint32_t c = esp_cpu_get_cycle_count();
    twai_initiate_recovery();
    all = 0;
    while (!(all & TWAI_ALERT_BUS_RECOVERED) && esp_cpu_get_cycle_count() - c < 240000u * 100)
        if (twai_read_alerts(&a, pdMS_TO_TICKS(1)) == ESP_OK) all |= a;
    printf("recovered alert after %lu us\n", (esp_cpu_get_cycle_count() - c) / 240);
    printf("idf_twai_ext done\n");
}
