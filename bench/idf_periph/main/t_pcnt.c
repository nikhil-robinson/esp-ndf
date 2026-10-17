#include "t.h"
#include "driver/pulse_cnt.h"
#include "driver/mcpwm_prelude.h"
#include "esp_rom_sys.h"

/* 1 MHz quadrature on GPIO4/5 from two synced IDF MCPWM timers */
static void quad(void)
{
    mcpwm_timer_handle_t t[2]; mcpwm_oper_handle_t o[2]; mcpwm_cmpr_handle_t c[2]; mcpwm_gen_handle_t g[2];
    for (int i = 0; i < 2; i++) {
        mcpwm_timer_config_t tc = { .group_id = 1, .clk_src = MCPWM_TIMER_CLK_SRC_DEFAULT, .resolution_hz = 160000000,
                                    .count_mode = MCPWM_TIMER_COUNT_MODE_UP, .period_ticks = 160 };
        mcpwm_new_timer(&tc, &t[i]);
        mcpwm_operator_config_t oc = { .group_id = 1 }; mcpwm_new_operator(&oc, &o[i]);
        mcpwm_operator_connect_timer(o[i], t[i]);
        mcpwm_comparator_config_t cc = { .flags.update_cmp_on_tez = true }; mcpwm_new_comparator(o[i], &cc, &c[i]);
        mcpwm_comparator_set_compare_value(c[i], 80);
        mcpwm_generator_config_t gc = { .gen_gpio_num = 4 + i, .flags.io_loop_back = 1 }; mcpwm_new_generator(o[i], &gc, &g[i]);
        mcpwm_generator_set_action_on_timer_event(g[i], MCPWM_GEN_TIMER_EVENT_ACTION(MCPWM_TIMER_DIRECTION_UP, MCPWM_TIMER_EVENT_EMPTY, MCPWM_GEN_ACTION_HIGH));
        mcpwm_generator_set_action_on_compare_event(g[i], MCPWM_GEN_COMPARE_EVENT_ACTION(MCPWM_TIMER_DIRECTION_UP, c[i], MCPWM_GEN_ACTION_LOW));
    }
    mcpwm_sync_handle_t s; mcpwm_timer_sync_src_config_t sc = { .timer_event = MCPWM_TIMER_EVENT_EMPTY };
    mcpwm_new_timer_sync_src(t[0], &sc, &s);
    mcpwm_timer_sync_phase_config_t pc = { .sync_src = s, .count_value = 120, .direction = MCPWM_TIMER_DIRECTION_UP };
    mcpwm_timer_set_phase_on_sync(t[1], &pc);
    for (int i = 0; i < 2; i++) { mcpwm_timer_enable(t[i]); mcpwm_timer_start_stop(t[i], MCPWM_TIMER_START_NO_STOP); }
    esp_rom_delay_us(1000);
}

void t_pcnt(void)
{
    pcnt_unit_config_t uc = { .high_limit = 30000, .low_limit = -30000, .flags.accum_count = 1 };
    pcnt_unit_handle_t u; pcnt_new_unit(&uc, &u);
    pcnt_chan_config_t a = { .edge_gpio_num = 4, .level_gpio_num = 5 }, b = { .edge_gpio_num = 5, .level_gpio_num = 4 };
    pcnt_channel_handle_t ca, cb; pcnt_new_channel(u, &a, &ca); pcnt_new_channel(u, &b, &cb);
    pcnt_channel_set_edge_action(ca, PCNT_CHANNEL_EDGE_ACTION_INCREASE, PCNT_CHANNEL_EDGE_ACTION_DECREASE);
    pcnt_channel_set_level_action(ca, PCNT_CHANNEL_LEVEL_ACTION_INVERSE, PCNT_CHANNEL_LEVEL_ACTION_KEEP);
    pcnt_channel_set_edge_action(cb, PCNT_CHANNEL_EDGE_ACTION_INCREASE, PCNT_CHANNEL_EDGE_ACTION_DECREASE);
    pcnt_channel_set_level_action(cb, PCNT_CHANNEL_LEVEL_ACTION_KEEP, PCNT_CHANNEL_LEVEL_ACTION_INVERSE);
    pcnt_unit_add_watch_point(u, 30000); pcnt_unit_add_watch_point(u, -30000);
    pcnt_unit_enable(u); pcnt_unit_start(u);
    quad();
    pcnt_unit_clear_count(u);
    int prev = 0, back = 0, v;
    uint32_t t = ccount();
    while (ccount() - t < 24000000) { pcnt_unit_get_count(u, &v); if (v < prev) back++; prev = v; }
    pcnt_unit_get_count(u, &v);
    METRIC("1 MHz fwd, 100 ms (expect 400000)", "%d", v);
    METRIC("backward steps while wrapping", "%d", back);
    METRIC("pcnt_get()", "%lu cycles", CYCLES(pcnt_unit_get_count(u, &v)));
}
