/* IDF sibling of components/drivers/*/test_apps/*_unit/test_mcpwm_ext.c: 1 kHz PWM on GPIO12 -> jumper -> capture on 13. */
#include <stdio.h>
#include "driver/mcpwm_prelude.h"
#include "esp_cpu.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static volatile uint32_t last, d, n;
static bool on_cap(mcpwm_cap_channel_handle_t c, const mcpwm_capture_event_data_t *e, void *a)
{
    if (n++) d = e->cap_value - last;
    last = e->cap_value;
    return false;
}

void app_main(void)
{
    mcpwm_timer_handle_t tm;
    mcpwm_timer_config_t tc = { .group_id = 1, .clk_src = MCPWM_TIMER_CLK_SRC_DEFAULT, .resolution_hz = 40000000,
                                .count_mode = MCPWM_TIMER_COUNT_MODE_UP, .period_ticks = 40000 };
    mcpwm_new_timer(&tc, &tm);
    mcpwm_oper_handle_t op; mcpwm_operator_config_t oc = { .group_id = 1 };
    mcpwm_new_operator(&oc, &op); mcpwm_operator_connect_timer(op, tm);
    mcpwm_cmpr_handle_t cmp; mcpwm_comparator_config_t cc = { .flags.update_cmp_on_tez = true };
    mcpwm_new_comparator(op, &cc, &cmp);
    mcpwm_comparator_set_compare_value(cmp, 20000);
    mcpwm_gen_handle_t g; mcpwm_generator_config_t gc = { .gen_gpio_num = 12 };
    mcpwm_new_generator(op, &gc, &g);
    mcpwm_generator_set_action_on_timer_event(g, MCPWM_GEN_TIMER_EVENT_ACTION(MCPWM_TIMER_DIRECTION_UP, MCPWM_TIMER_EVENT_EMPTY, MCPWM_GEN_ACTION_HIGH));
    mcpwm_generator_set_action_on_compare_event(g, MCPWM_GEN_COMPARE_EVENT_ACTION(MCPWM_TIMER_DIRECTION_UP, cmp, MCPWM_GEN_ACTION_LOW));
    mcpwm_timer_enable(tm); mcpwm_timer_start_stop(tm, MCPWM_TIMER_START_NO_STOP);

    uint32_t best = ~0u;
    for (int k = 0; k < 16; k++) {
        uint32_t c = esp_cpu_get_cycle_count();
        mcpwm_comparator_set_compare_value(cmp, 20000 + k);
        c = esp_cpu_get_cycle_count() - c;
        if (c < best) best = c;
    }
    printf("set compare: %lu cycles\n", best);

    mcpwm_cap_timer_handle_t ct; mcpwm_capture_timer_config_t ctc = { .group_id = 1, .clk_src = MCPWM_CAPTURE_CLK_SRC_DEFAULT };
    mcpwm_new_capture_timer(&ctc, &ct);
    mcpwm_cap_channel_handle_t ch;
    mcpwm_capture_channel_config_t chc = { .gpio_num = 13, .prescale = 4, .flags.pos_edge = true };
    mcpwm_new_capture_channel(ct, &chc, &ch);
    mcpwm_capture_event_callbacks_t cb = { .on_cap = on_cap };
    mcpwm_capture_channel_register_event_callbacks(ch, &cb, NULL);
    mcpwm_capture_channel_enable(ch); mcpwm_capture_timer_enable(ct); mcpwm_capture_timer_start(ct);
    vTaskDelay(pdMS_TO_TICKS(40));
    printf("capture prescale 4: %lu captures in 40 ms, delta %lu ticks (1 period = 80000)\n", n, d);
    printf("idf_mcpwm_ext done\n");
}
