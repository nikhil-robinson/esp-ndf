/* IDF sibling of components/drivers/*/test_apps/*_unit/test_pcnt_ext.c: quadrature on 13/11 + watch points; read cost. */
#include <stdio.h>
#include "driver/pulse_cnt.h"
#include "esp_cpu.h"

void app_main(void)
{
    pcnt_unit_config_t uc = { .low_limit = -30000, .high_limit = 30000 };
    pcnt_unit_handle_t u;
    pcnt_new_unit(&uc, &u);
    pcnt_chan_config_t a = { .edge_gpio_num = 13, .level_gpio_num = 11 }, b = { .edge_gpio_num = 11, .level_gpio_num = 13 };
    pcnt_channel_handle_t ca, cb;
    pcnt_new_channel(u, &a, &ca); pcnt_new_channel(u, &b, &cb);
    pcnt_channel_set_edge_action(ca, PCNT_CHANNEL_EDGE_ACTION_DECREASE, PCNT_CHANNEL_EDGE_ACTION_INCREASE);
    pcnt_channel_set_level_action(ca, PCNT_CHANNEL_LEVEL_ACTION_KEEP, PCNT_CHANNEL_LEVEL_ACTION_INVERSE);
    pcnt_unit_add_watch_point(u, 25);
    pcnt_unit_enable(u); pcnt_unit_clear_count(u); pcnt_unit_start(u);
    int v; uint32_t best = ~0u;
    for (int k = 0; k < 16; k++) {
        uint32_t c = esp_cpu_get_cycle_count();
        pcnt_unit_get_count(u, &v);
        c = esp_cpu_get_cycle_count() - c;
        if (c < best) best = c;
    }
    printf("pcnt_unit_get_count: %lu cycles\n", best);
    printf("idf_pcnt_ext done\n");
}
