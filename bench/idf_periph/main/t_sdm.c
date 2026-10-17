#include "t.h"
#include "driver/sdm.h"
#include "driver/gpio.h"
void t_sdm(void)
{
    sdm_channel_handle_t c;
    sdm_config_t cfg = { .clk_src = SDM_CLK_SRC_DEFAULT, .sample_rate_hz = 10000000, .gpio_num = 4, .flags.io_loop_back = 1 };
    sdm_new_channel(&cfg, &c); sdm_channel_enable(c);
    static const int8_t d[3] = { -64, 0, 64 };
    for (int k = 0; k < 3; k++) {
        sdm_channel_set_pulse_density(c, d[k]);
        uint32_t hi = 0;
        for (int i = 0; i < 200000; i++) hi += gpio_get_level(4);
        printf("    density %4d: duty %lu.%lu %%\n", d[k], hi / 2000, (hi / 200) % 10);
    }
    METRIC("sdm_density()", "%lu cycles", CYCLES(sdm_channel_set_pulse_density(c, 10)));
}
