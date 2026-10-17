#include "t.h"
#include "driver/gptimer.h"

typedef struct { uint32_t n, last, jmax, lat_min, lat_max, period; gptimer_handle_t h; } st_t;
static st_t st;

static bool IRAM_ATTR cb(gptimer_handle_t t, const gptimer_alarm_event_data_t *e, void *arg)
{
    st_t *s = arg;
    uint32_t now = ccount();
    uint64_t c; gptimer_get_raw_count(t, &c);
    if (c < s->lat_min) s->lat_min = c;
    if (c > s->lat_max) s->lat_max = c;
    if (s->n && s->last) {
        int32_t d = (int32_t)(now - s->last - s->period);
        uint32_t j = d < 0 ? -d : d;
        if (j > s->jmax) s->jmax = j;
    }
    s->last = now; s->n++;
    return false;
}

void t_gptimer(void)
{
    gptimer_handle_t t;
    gptimer_config_t c = { .clk_src = GPTIMER_CLK_SRC_APB, .direction = GPTIMER_COUNT_UP, .resolution_hz = 1000000 };
    gptimer_new_timer(&c, &t);
    gptimer_enable(t); gptimer_start(t);
    uint64_t v;
    METRIC("gpt_get()", "%lu cycles", CYCLES(gptimer_get_raw_count(t, &v)));
    gptimer_stop(t); gptimer_disable(t); gptimer_del_timer(t);

    c.resolution_hz = 40000000;
    gptimer_new_timer(&c, &t);
    st = (st_t){ .period = 24000, .lat_min = ~0u };
    gptimer_alarm_config_t a = { .alarm_count = 4000, .flags.auto_reload_on_alarm = true };
    gptimer_set_alarm_action(t, &a);
    gptimer_event_callbacks_t cbs = { .on_alarm = cb };
    gptimer_register_event_callbacks(t, &cbs, &st);
    gptimer_enable(t); gptimer_start(t);
    while (*(volatile uint32_t *)&st.n < 5000) {}
    gptimer_stop(t);
    METRIC("alarm -> callback (L1)", "min %lu max %lu ns", st.lat_min * 25, st.lat_max * 25);
    METRIC("10 kHz period jitter (L1)", "max %lu cycles", st.jmax);
}
