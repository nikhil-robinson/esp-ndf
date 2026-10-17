#include "t.h"
#include "driver/twai.h"
#include "driver/gpio.h"
static void run(twai_timing_config_t t, int kbps)
{
    twai_general_config_t g = TWAI_GENERAL_CONFIG_DEFAULT(10, 11, TWAI_MODE_NO_ACK);
    twai_filter_config_t f = TWAI_FILTER_CONFIG_ACCEPT_ALL();
    twai_driver_install(&g, &t, &f); twai_start();
    int ok = 0;
    for (int i = 0; i < 100; i++) {
        twai_message_t m = { .identifier = i & 1 ? 0x1ABCDE00 + i : 0x100 + i, .extd = i & 1, .data_length_code = i % 9, .self = 1 }, r;
        for (int k = 0; k < 8; k++) m.data[k] = i * 8 + k;
        if (twai_transmit(&m, 10) != ESP_OK || twai_receive(&r, 10) != ESP_OK) continue;
        bool same = r.identifier == m.identifier && r.extd == m.extd && r.data_length_code == m.data_length_code;
        for (int k = 0; k < m.data_length_code; k++) same &= r.data[k] == m.data[k];
        ok += same;
    }
    printf("    %4d kbit/s: %d/100 frames round-trip identical\n", kbps, ok);
    if (kbps == 1000) {
        twai_message_t m = { .identifier = 1, .data_length_code = 8, .self = 1 }, r;
        uint32_t c = ccount(); twai_transmit(&m, 10); while (twai_receive(&r, 0) != ESP_OK) {} c = ccount() - c;
        METRIC("twai_send() 8 bytes @1M (wire incl.)", "%lu cycles", c);
    }
    twai_stop(); twai_driver_uninstall();
}
void t_twai(void)
{
    gpio_reset_pin(10); gpio_reset_pin(11);
    run((twai_timing_config_t)TWAI_TIMING_CONFIG_125KBITS(), 125);
    run((twai_timing_config_t)TWAI_TIMING_CONFIG_1MBITS(), 1000);
    run((twai_timing_config_t)TWAI_TIMING_CONFIG_500KBITS(), 500);
}
