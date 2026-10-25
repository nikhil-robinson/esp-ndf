/* IDF sibling of components/drivers/*/test_apps/*_unit test_touch_ext: pad 10, filter on, read costs. */
#include <stdio.h>
#include "driver/touch_pad.h"
#include "esp_cpu.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static uint32_t spread(void)
{
    uint32_t lo = ~0u, hi = 0, v;
    for (int i = 0; i < 200; i++) { touch_pad_read_raw_data(TOUCH_PAD_NUM10, &v); if (v < lo) lo = v; if (v > hi) hi = v; vTaskDelay(1); }
    return hi - lo;
}

static void denoise(void)                                  /* same as periph_test touch_denoise: 4-bit grade, 5 pF */
{
    uint32_t r0, r1, dn, s0 = spread();
    touch_pad_read_raw_data(TOUCH_PAD_NUM10, &r0);
    touch_pad_denoise_t d = { .grade = TOUCH_PAD_DENOISE_BIT4, .cap_level = TOUCH_PAD_DENOISE_CAP_L0 };
    touch_pad_denoise_set_config(&d); touch_pad_denoise_enable();
    vTaskDelay(pdMS_TO_TICKS(100));
    uint32_t s1 = spread();
    touch_pad_read_raw_data(TOUCH_PAD_NUM10, &r1); touch_pad_denoise_read_data(&dn);
    printf("denoise: pad 10 raw %lu spread %lu -> on: raw %lu spread %lu, reference %lu\n", r0, s0, r1, s1, dn);
}

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
    denoise();
    printf("idf_touch_ext denoise done\n");
}
