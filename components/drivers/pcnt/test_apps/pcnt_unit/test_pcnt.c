#include "test.h"
#include "pcnt.h"
#include "mcpwm.h"
#include "gpio.h"
#include "sys.h"

#define PA 4
#define PB 5

/* Quadrature on PA/PB from two synced MCPWM timers; B lags A by 90 deg
 * (forward) or leads (reverse). */
static void quad(uint32_t hz, bool forward)
{
    uint32_t per = mcpwm_timer(0, 0, hz, MCPWM_UP, 0);
    mcpwm_timer(0, 1, hz, MCPWM_UP, 0);
    mcpwm_pwm(0, 0, 0, PA, -1); mcpwm_cmp(0, 0, 0, per / 2);
    mcpwm_pwm(0, 1, 1, PB, -1); mcpwm_cmp(0, 1, 0, per / 2);
    mcpwm_sync(0, 1, 0, forward ? per - per / 4 : per / 4);
    mcpwm_start(0, 1); mcpwm_start(0, 0);
    delay_us(1000);                 /* until timer 1 locks to timer 0's sync, phase is arbitrary */
}

static void quad_stop(void) { mcpwm_stop(0, 0); mcpwm_stop(0, 1); }

void test_pcnt(void)
{
    __asm__ volatile("rsil a2, 0" ::: "a2");
    CHECK(pcnt_quadrature(0, PA, PB) == 0);

    /* 20 kHz for 10 ms: 200 periods x 4 = 800 counts (+-4 for start/stop phase) */
    quad(20000, true);
    pcnt_clear(0);
    uint32_t t = ccount(); while (ccount() - t < 2400000) {}
    int32_t f = pcnt_get(0);
    quad_stop();
    METRIC("20 kHz fwd, 10 ms (expect 800)", "%ld", f);
    CHECK(f >= 796 && f <= 804);

    quad(20000, false);
    pcnt_clear(0);
    t = ccount(); while (ccount() - t < 2400000) {}
    int32_t r = pcnt_get(0);
    quad_stop();
    METRIC("20 kHz rev, 10 ms (expect -800)", "%ld", r);
    CHECK(r <= -796 && r >= -804);

    /* 1 MHz for 100 ms: 400000 counts, ~13 hardware wraps; read while running
     * and check the count never steps backwards */
    quad(1000000, true);
    pcnt_clear(0);
    int32_t prev = 0, back = 0;
    t = ccount();
    while (ccount() - t < 24000000) { int32_t v = pcnt_get(0); if (v < prev) back++; prev = v; }
    int32_t big = pcnt_get(0);
    quad_stop();
    METRIC("1 MHz fwd, 100 ms (expect 400000)", "%ld", big);
    METRIC("backward steps while wrapping", "%ld", back);
    CHECK(big >= 399990 && big <= 400010);
    CHECK(back == 0);
    METRIC("pcnt_get()", "%lu cycles", CYCLES(pcnt_get(0)));
    /* 100 kHz for 1 s with no reads in between: wraps counted by the IRQ alone */
    quad(100000, true);
    pcnt_clear(0);
    t = ccount(); for (int k = 0; k < 10; k++) { uint32_t t1 = ccount(); while (ccount() - t1 < 24000000) {} }
    int32_t slow = pcnt_get(0);
    quad_stop();
    METRIC("100 kHz, 1 s, IRQ-only (expect 400000)", "%ld", slow);
    CHECK(slow >= 399996 && slow <= 400004);
    gpio_config(PA, GPIO_OUTPUT | GPIO_INPUT); gpio_config(PB, GPIO_OUTPUT | GPIO_INPUT);
}

NDF_UNIT(010, pcnt);
