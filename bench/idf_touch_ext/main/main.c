/* IDF sibling of examples/periph_test test_touch_ext: pad 10, filter on, read costs. */
#include <stdio.h>
#include "driver/touch_pad.h"
#include "esp_cpu.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

void app_main(void)
{
    touch_pad_init();
    touch_pad_config(TOUCH_PAD_NUM10);
    touch_filter_config_t f = { .mode = TOUCH_PAD_FILTER_IIR_16, .debounce_cnt = 1, .noise_thr = 0, .jitter_step = 4, .smh_lvl = TOUCH_PAD_SMOOTH_IIR_2 };
    touch_pad_filter_set_config(&f); touch_pad_filter_enable();
    touch_pad_set_fsm_mode(TOUCH_FSM_MODE_TIMER); touch_pad_fsm_start();
    vTaskDelay(pdMS_TO_TICKS(200));
    uint32_t v, b, best = ~0u, bestb = ~0u;
    for (int k = 0; k < 16; k++) {
        uint32_t c = esp_cpu_get_cycle_count(); touch_pad_read_raw_data(TOUCH_PAD_NUM10, &v); c = esp_cpu_get_cycle_count() - c;
        if (c < best) best = c;
        c = esp_cpu_get_cycle_count(); touch_pad_read_benchmark(TOUCH_PAD_NUM10, &b); c = esp_cpu_get_cycle_count() - c;
        if (c < bestb) bestb = c;
    }
    printf("raw %lu (read %lu cycles), benchmark %lu (read %lu cycles)\n", v, best, b, bestb);
    printf("idf_touch_ext done\n");
}
