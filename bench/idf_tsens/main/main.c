/* IDF sibling of components/drivers/tsens/test_apps/tsens_test. */
#include <stdio.h>
#include "driver/temperature_sensor.h"
#include "esp_cpu.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

void app_main(void)
{
    temperature_sensor_handle_t t;
    temperature_sensor_config_t c = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
    temperature_sensor_install(&c, &t);
    temperature_sensor_enable(t);
    float s = 0, v; uint32_t best = ~0u;
    for (int i = 0; i < 10; i++) {
        uint32_t cy = esp_cpu_get_cycle_count(); temperature_sensor_get_celsius(t, &v); cy = esp_cpu_get_cycle_count() - cy;
        if (cy < best) best = cy;
        s += v; vTaskDelay(pdMS_TO_TICKS(50));
    }
    printf("idf_tsens: mean %.1f C, read %lu cycles\n", s / 10, best);
    printf("idf_tsens done\n");
}
