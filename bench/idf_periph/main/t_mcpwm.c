#include "t.h"
#include "driver/mcpwm_prelude.h"
static volatile uint32_t n, last, jmax;
static bool IRAM_ATTR on_empty(mcpwm_timer_handle_t t, const mcpwm_timer_event_data_t *e, void *a)
{
    uint32_t now = ccount();
    if (n > 1) { int32_t d = (int32_t)(now - last - 12000); uint32_t j = d < 0 ? -d : d; if (j > jmax) jmax = j; }
    last = now; n++;
    return false;
}
void t_mcpwm(void)
{
    mcpwm_timer_handle_t t;
    mcpwm_timer_config_t tc = { .group_id = 0, .clk_src = MCPWM_TIMER_CLK_SRC_DEFAULT, .resolution_hz = 160000000,
                                .count_mode = MCPWM_TIMER_COUNT_MODE_UP, .period_ticks = 8000 };
    mcpwm_new_timer(&tc, &t);
    mcpwm_oper_handle_t op; mcpwm_operator_config_t oc = { .group_id = 0 };
    mcpwm_new_operator(&oc, &op); mcpwm_operator_connect_timer(op, t);
    mcpwm_cmpr_handle_t cmp; mcpwm_comparator_config_t cc = { .flags.update_cmp_on_tez = true };
    mcpwm_new_comparator(op, &cc, &cmp);
    mcpwm_gen_handle_t g; mcpwm_generator_config_t gc = { .gen_gpio_num = 4 };
    mcpwm_new_generator(op, &gc, &g);
    mcpwm_timer_event_callbacks_t cbs = { .on_empty = on_empty };
    mcpwm_timer_register_event_callbacks(t, &cbs, 0);
    mcpwm_timer_enable(t); mcpwm_timer_start_stop(t, MCPWM_TIMER_START_NO_STOP);
    METRIC("mcpwm_cmp()", "%lu cycles", CYCLES(mcpwm_comparator_set_compare_value(cmp, 3000)));
    while (n < 5000) {}
    METRIC("period IRQ jitter (20 kHz, L1)", "max %lu cycles", jmax);
    mcpwm_timer_start_stop(t, MCPWM_TIMER_STOP_EMPTY);
}
