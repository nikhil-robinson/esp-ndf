/* IDF sibling of components/drivers/*/test_apps/*_unit test_adc_filter: ADC1 ch9 (GPIO10) <- jumper <- GPIO11 driven high.
 * Monitor high threshold 3000 on ch9: count events in 100 ms. */
#include <stdio.h>
#include "esp_adc/adc_continuous.h"
#include "esp_adc/adc_monitor.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static volatile int hi;
static bool on_hi(adc_monitor_handle_t m, const adc_monitor_evt_data_t *e, void *a) { hi++; return false; }

void app_main(void)
{
    gpio_set_direction(11, GPIO_MODE_OUTPUT); gpio_set_level(11, 1);
    adc_continuous_handle_t h;
    adc_continuous_handle_cfg_t hc = { .max_store_buf_size = 4096, .conv_frame_size = 1024 };
    adc_continuous_new_handle(&hc, &h);
    adc_digi_pattern_config_t p = { .atten = ADC_ATTEN_DB_12, .channel = 9, .unit = ADC_UNIT_1, .bit_width = 12 };
    adc_continuous_config_t c = { .pattern_num = 1, .adc_pattern = &p, .sample_freq_hz = 20000,
                                  .conv_mode = ADC_CONV_SINGLE_UNIT_1, .format = ADC_DIGI_OUTPUT_FORMAT_TYPE2 };
    adc_continuous_config(h, &c);
    adc_monitor_handle_t m;
    adc_monitor_config_t mc = { .adc_unit = ADC_UNIT_1, .channel = 9, .h_threshold = 3000, .l_threshold = -1 };
    printf("monitor new %d\n", adc_new_continuous_monitor(h, &mc, &m));
    adc_monitor_evt_cbs_t cb = { .on_over_high_thresh = on_hi };
    adc_continuous_monitor_register_event_callbacks(m, &cb, NULL);
    adc_continuous_monitor_enable(m);
    adc_continuous_start(h);
    vTaskDelay(pdMS_TO_TICKS(100));
    int n = hi;
    adc_continuous_stop(h);
    printf("ch9 high (GPIO11 = 1), monitor ch9 > 3000: %d events in 100 ms (2000 samples)\n", n);
    printf("idf_adc_ext done\n");
}
