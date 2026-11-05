#include "test.h"
#include "mcpwm.h"
#include "pcnt.h"
#include "gpio.h"
#include "sys.h"

/* PWM A out on 12 -> jumper -> read back on 13. Fault 0 driven from pad 10 -> jumper -> pad 11. */
#define GPIO_IN 0x6000403C
static int ones(uint32_t us)                               /* samples of pad 13 that are high */
{
    int k = 0; uint32_t t0 = ccount();
    while (ccount() - t0 < us * (sys_cpu_hz() / 1000000)) k += (REG(GPIO_IN) >> 13) & 1;
    return k;
}

static volatile uint32_t cap_last, cap_d, cap_n;
static void on_cap(int ch, uint32_t t, int edge, void *a)
{
    (void)ch; (void)edge; (void)a;
    if (cap_n++) cap_d = t - cap_last;
    cap_last = t;
}

void test_mcpwm_ext(void)
{
    irq_enable_all();
    uint32_t per = mcpwm_timer(1, 0, 10000, MCPWM_UP, 0);   /* unit 1: unit 0 is used by test_mcpwm */
    mcpwm_pwm(1, 0, 0, 12, -1);
    mcpwm_cmp(1, 0, 0, per / 2);
    mcpwm_start(1, 0);
    gpio_config(13, GPIO_INPUT);
    gpio_config(10, GPIO_OUTPUT); gpio_write(10, 0);
    mcpwm_fault_pin(1, 0, 11, true);
    int run = ones(2000);

    mcpwm_fault_action(1, 0, 0, true, 0);                  /* one-shot: latch low */
    gpio_write(10, 1); delay_us(200);
    int trip = ones(2000), act = mcpwm_fault_active(1, 0);
    gpio_write(10, 0); delay_us(200);
    int latched = ones(2000);
    mcpwm_force(1, 0, -1); delay_us(200);
    int resumed = ones(2000);
    printf("    one-shot fault: running %d, tripped %d (event %d), fault gone %d (latched), released %d\n",
           run, trip, act, latched, resumed);
    CHECK(run > 1000 && trip == 0 && act && latched == 0 && resumed > 1000);

    mcpwm_fault_action(1, 0, 0, false, 0);                 /* cycle by cycle */
    gpio_write(10, 1); delay_us(200);
    int cbc = ones(2000);
    gpio_write(10, 0); delay_us(300);
    int back = ones(2000);
    printf("    cycle-by-cycle fault: tripped %d, fault gone %d (back by itself)\n", cbc, back);
    CHECK(cbc == 0 && back > 1000);
    REG(0x6002C068) = 0;                                   /* op0 fault handler off */

    /* carrier on 1 kHz 50 % PWM: count pulses on 13 for 10 ms */
    per = mcpwm_timer(1, 0, 1000, MCPWM_UP, 0);             /* re-time the running timer: 10 kHz -> 1 kHz */
    mcpwm_cmp(1, 0, 0, per / 2);
    mcpwm_start(1, 0);
    uint32_t chz = mcpwm_carrier(1, 0, 1250000, 4, 1);
    pcnt_channel(0, 0, 13, -1, PCNT_EDGE_UP, PCNT_EDGE_OFF, PCNT_CTRL_KEEP, PCNT_CTRL_KEEP);
    pcnt_clear(0); delay_us(10000);
    int32_t pulses = pcnt_get(0);
    mcpwm_carrier(1, 0, 0, 0, 0);
    pcnt_clear(0); delay_us(10000);
    int32_t plain = pcnt_get(0);
    printf("    carrier %lu Hz on 1 kHz PWM: %ld pulses in 10 ms (ideal 6250), without carrier %ld\n", chz, pulses, plain);
    CHECK(chz == 1250000 && pulses > 6000 && pulses < 6500 && plain >= 9 && plain <= 11);

    /* capture prescale: every 4th rising edge of the 1 kHz PWM -> 4 ms apart */
    CHECK(mcpwm_capture_prescale(1, 0, 4) == 4);
    cap_n = 0;
    mcpwm_capture(1, 0, 13, MCPWM_POS, on_cap, 0, 1);
    delay_us(30000);
    mcpwm_capture(1, 0, 13, MCPWM_POS, 0, 0, 1);
    printf("    capture /4: %lu captures in 30 ms, delta %lu ticks (4 ms = 320000)\n", cap_n, cap_d);
    CHECK(cap_n >= 6 && cap_n <= 9 && cap_d > 319000 && cap_d < 321000);
    mcpwm_capture_prescale(1, 0, 1);
    mcpwm_stop(1, 0);
}

NDF_UNIT(020, mcpwm_ext);
