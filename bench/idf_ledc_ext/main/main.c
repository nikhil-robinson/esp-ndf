/* IDF sibling for components/drivers/*/test_apps/*_unit test_ledc / test_ledc_gamma: duty update cost (S3 IDF has no gamma fade). */
#include <stdio.h>
#include "driver/ledc.h"
#include "esp_cpu.h"

void app_main(void)
{
    ledc_timer_config_t t = { .speed_mode = LEDC_LOW_SPEED_MODE, .duty_resolution = LEDC_TIMER_10_BIT, .timer_num = 0,
                              .freq_hz = 20000, .clk_cfg = LEDC_USE_APB_CLK };
    ledc_timer_config(&t);
    ledc_channel_config_t c = { .gpio_num = 4, .speed_mode = LEDC_LOW_SPEED_MODE, .channel = 0, .timer_sel = 0, .duty = 256 };
    ledc_channel_config(&c);
    uint32_t best = ~0u;
    for (int k = 0; k < 16; k++) {
        uint32_t cy = esp_cpu_get_cycle_count();
        ledc_set_duty(LEDC_LOW_SPEED_MODE, 0, 300 + k);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, 0);
        cy = esp_cpu_get_cycle_count() - cy;
        if (cy < best) best = cy;
    }
    printf("ledc_set_duty + update: %lu cycles\n", best);
    printf("idf_ledc_ext done\n");
}
