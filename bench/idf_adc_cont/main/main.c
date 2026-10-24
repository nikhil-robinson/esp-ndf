/* IDF sibling of examples/periph_test test_adc_cont: ADC1 ch9, 20 kHz, 1000-sample frames; frame interval jitter. */
#include <stdio.h>
#include "esp_adc/adc_continuous.h"
#include "esp_cpu.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static volatile uint32_t t[32]; static volatile int nf;
static bool IRAM_ATTR done(adc_continuous_handle_t h, const adc_continuous_evt_data_t *e, void *a)
{
    if (nf < 32) t[nf] = esp_cpu_get_cycle_count();
    nf++;
    return false;
}

void app_main(void)
{
    adc_continuous_handle_t h;
    adc_continuous_handle_cfg_t hc = { .max_store_buf_size = 16384, .conv_frame_size = 4000 };   /* 1000 x 4 B (TYPE2) */
    adc_continuous_new_handle(&hc, &h);
    adc_digi_pattern_config_t p = { .atten = ADC_ATTEN_DB_12, .channel = 9, .unit = ADC_UNIT_1, .bit_width = 12 };
    adc_continuous_config_t c = { .pattern_num = 1, .adc_pattern = &p, .sample_freq_hz = 20000,
                                  .conv_mode = ADC_CONV_SINGLE_UNIT_1, .format = ADC_DIGI_OUTPUT_FORMAT_TYPE2 };
    adc_continuous_config(h, &c);
    adc_continuous_evt_cbs_t cb = { .on_conv_done = done };
    adc_continuous_register_event_callbacks(h, &cb, NULL);
    adc_continuous_start(h);
    while (nf < 14) { static uint8_t b[4000]; uint32_t n; adc_continuous_read(h, b, sizeof b, &n, 0); vTaskDelay(1); }
    adc_continuous_stop(h);
    uint32_t mn = ~0u, mx = 0;
    for (int i = 3; i < 14; i++) { uint32_t d = t[i] - t[i - 1]; if (d < mn) mn = d; if (d > mx) mx = d; }
    printf("idf_adc_cont: frame interval %lu..%lu cycles (50 ms = 12000000), jitter %lu\n", mn, mx, mx - mn);
    printf("idf_adc_cont done\n");
}
