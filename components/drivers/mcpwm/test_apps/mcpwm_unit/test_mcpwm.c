#include "test.h"
#include "mcpwm.h"
#include "gpio.h"
#include "sys.h"

#if SOC_CPU_RISCV
#define K 2              /* capture timer ticks per S3 tick: the P4 captures at the 160 MHz group clock, the S3 at 80 MHz APB */
#else
#define K 1
#endif
#define PA 4
#define PB 5

typedef struct { volatile uint32_t n; uint32_t t[8]; uint8_t e[8]; } cap_t;
static cap_t cap[2];

RAM_FN static void on_cap(int ch, uint32_t ticks, int edge, void *arg)
{
    cap_t *c = arg; (void)ch;
    if (c->n < 8) { c->t[c->n] = ticks; c->e[c->n] = edge; }
    c->n++;
}

/* rise->fall (high time) and rise->rise (period) in 12.5 ns capture ticks */
static void edges(cap_t *c, uint32_t *high, uint32_t *period)
{
    int i = 0;
    while (i < 6 && c->e[i] != MCPWM_POS) i++;
    *high = c->t[i + 1] - c->t[i];
    *period = c->t[i + 2] - c->t[i];
}

static void grab(int ch, int pin, cap_t *c)
{
    *c = (cap_t){ 0 };
    mcpwm_capture(0, ch, pin, MCPWM_BOTH, on_cap, c, 1);
}

static volatile uint32_t tez_n, tez_last, tez_jmax;
RAM_FN static void on_tez(void *a)
{
    uint32_t now = ccount(); (void)a;
    if (tez_n > 1) {
        int32_t d = (int32_t)(now - tez_last - 12000);
        uint32_t j = d < 0 ? -d : d;
        if (j > tez_jmax) tez_jmax = j;
    }
    tez_last = now; tez_n++;
}

void test_mcpwm(void)
{
    irq_enable_all();
    uint32_t tick;
    uint32_t per = mcpwm_timer(0, 0, 20000, MCPWM_UP, &tick);       /* 20 kHz edge */
    METRIC("20 kHz edge-aligned: period", "%lu ticks @ %lu MHz", per, tick / 1000000);
    CHECK(per == 8000 && tick == 160000000);
    CHECK(mcpwm_pwm(0, 0, 0, PA, -1) == 0);
    mcpwm_cmp(0, 0, 0, 2000);                                         /* 25 % */
    mcpwm_start(0, 0);
    grab(0, PA, &cap[0]);
    WAIT_FOR(cap[0].n >= 8, 100);
    uint32_t hi, pp;
    edges(&cap[0], &hi, &pp);
    METRIC("captured high / period", "%lu / %lu (12.5 ns ticks)", hi, pp);
    CHECK(hi == 1000 * K && pp == 4000 * K);                                  /* 12.5 us / 50 us exact */

    per = mcpwm_timer(0, 1, 20000, MCPWM_UPDOWN, &tick);             /* center aligned */
    CHECK(per == 4000);
    mcpwm_pwm(0, 1, 1, PB, -1);
    mcpwm_cmp(0, 1, 0, 1000);                                         /* 25 % */
    mcpwm_start(0, 1);
    grab(1, PB, &cap[1]);
    WAIT_FOR(cap[1].n >= 8, 100);
    edges(&cap[1], &hi, &pp);
    METRIC("center-aligned high / period", "%lu / %lu", hi, pp);
    CHECK(hi == 1000 * K && pp == 4000 * K);

    /* complementary 25 %, 1 us dead time each side (160 ticks @ 160 MHz) */
    mcpwm_stop(0, 1);
    mcpwm_complementary(0, 0, 0, PA, PB, 160, 160);
    mcpwm_cmp(0, 0, 0, 2000);
    grab(0, PA, &cap[0]); grab(1, PB, &cap[1]);
    WAIT_FOR(cap[0].n >= 8 && cap[1].n >= 8, 100);
    METRIC("complementary edges A / B", "%lu / %lu", cap[0].n, cap[1].n);
    /* dead band, both directions: smallest positive A-fall->B-rise and
     * B-fall->A-rise over all recorded edges (12.5 ns ticks; 80 = 1 us) */
    uint32_t db1 = ~0u, db2 = ~0u;
    for (int i = 0; i < 8; i++) for (int j = 0; j < 8; j++) {
        int32_t d = (int32_t)(cap[1].t[j] - cap[0].t[i]);
        if (cap[0].e[i] == MCPWM_NEG && cap[1].e[j] == MCPWM_POS && d > 0 && (uint32_t)d < db1) db1 = d;
        d = (int32_t)(cap[0].t[i] - cap[1].t[j]);
        if (cap[1].e[j] == MCPWM_NEG && cap[0].e[i] == MCPWM_POS && d > 0 && (uint32_t)d < db2) db2 = d;
    }
    METRIC("dead band A->B / B->A", "%lu / %lu ticks (80 = 1 us)", db1, db2);
    CHECK(db1 >= 79 * K && db1 <= 81 * K + (K - 1) && db2 >= 79 * K && db2 <= 81 * K + (K - 1));

    mcpwm_force(0, 0, 0);                                             /* brake: both low */
    delay_us(200);
    CHECK(gpio_read(PA) == 0 && gpio_read(PB) == 0);
    mcpwm_force(0, 0, -1);
    METRIC("mcpwm_cmp()", "%lu cycles", CYCLES(mcpwm_cmp(0, 0, 0, 3000)));

    /* 3-phase style sync: timer 2 locked to timer 0's zero, phase 2000 of
     * 8000 ticks (90 deg). Captured rising edges must be 1000 capture ticks apart. */
    mcpwm_force(0, 0, -1);
    mcpwm_pwm(0, 0, 0, PA, -1); mcpwm_cmp(0, 0, 0, 4000);
    mcpwm_timer(0, 2, 20000, MCPWM_UP, 0);
    mcpwm_pwm(0, 2, 2, PB, -1); mcpwm_cmp(0, 2, 0, 4000);
    mcpwm_sync(0, 2, 0, 8000 - 2000);                                  /* load 6000 at sync = lag 2000 */
    mcpwm_start(0, 2);
    grab(0, PA, &cap[0]); grab(1, PB, &cap[1]);
    WAIT_FOR(cap[0].n >= 8 && cap[1].n >= 8, 100);
    uint32_t lag = ~0u;
    for (int i = 0; i < 8; i++) for (int j = 0; j < 8; j++) {
        int32_t d = (int32_t)(cap[1].t[j] - cap[0].t[i]);
        if (cap[0].e[i] == MCPWM_POS && cap[1].e[j] == MCPWM_POS && d >= 0 && (uint32_t)d < lag) lag = d;
    }
    METRIC("synced timer phase lag", "%lu ticks (1000 = 90 deg)", lag);
    CHECK(lag >= 999 * K && lag <= 1001 * K);
    mcpwm_stop(0, 2);

    mcpwm_capture(0, 0, PA, MCPWM_BOTH, 0, 0, 1);                     /* captures off */
    mcpwm_capture(0, 1, PB, MCPWM_BOTH, 0, 0, 1);
    CHECK(mcpwm_on_period(0, 0, on_tez, 0, 1) == 0);
    WAIT_FOR(tez_n >= 5000, 1000);
    METRIC("period IRQ jitter (20 kHz, L1)", "max %lu cycles", tez_jmax);
    mcpwm_on_period(0, 0, 0, 0, 1);
    mcpwm_stop(0, 0);
    gpio_config(PA, GPIO_OUTPUT | GPIO_INPUT); gpio_config(PB, GPIO_OUTPUT | GPIO_INPUT);
}

NDF_UNIT(010, mcpwm);
