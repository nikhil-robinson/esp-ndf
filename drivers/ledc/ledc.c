/* LEDC (low-speed group; the S3 has no high-speed group). */
#include "ledc.h"
#include "gpio.h"
#include "clk.h"

#define LEDC_BASE        0x60019000
#define CH_CONF0(c)      (LEDC_BASE + 0x14 * (c) + 0x00)  /* b1:0 timer b2 out_en b3 idle b4 para_up */
#define CH_HPOINT(c)     (LEDC_BASE + 0x14 * (c) + 0x04)
#define CH_DUTY(c)       (LEDC_BASE + 0x14 * (c) + 0x08)  /* duty << 4 (4 dither bits) */
#define CH_CONF1(c)      (LEDC_BASE + 0x14 * (c) + 0x0C)  /* b31 start b30 inc b29:20 num b19:10 cycle b9:0 scale */
#define TM_CONF(t)       (LEDC_BASE + 0xA0 + 8 * (t))     /* b3:0 res b21:4 div(10.8) b22 pause b23 rst b25 para_up */
#define LEDC_CONF        (LEDC_BASE + 0xD0)               /* b1:0 clk sel (1 = APB) b31 clk_en */
#define LEDC_SIG_OUT0    73

static uint8_t tm_res[LEDC_TIMERS];
static uint8_t ch_tm[LEDC_CHANNELS];

uint64_t ledc_timer(int t, uint32_t hz, int res)
{
    if (t < 0 || t >= LEDC_TIMERS || res < 1 || res > 14 || !hz) return 0;
    /* div (x256) = 80 MHz * 256 / (hz << res), rounded to nearest */
    uint64_t den = (uint64_t)hz << res;
    uint64_t div = ((uint64_t)80000000 * 256 + den / 2) / den;
    if (div < 256 || div > 0x3FFFF) return 0;
    periph_enable(PERIPH_LEDC);
    REG(LEDC_CONF) = BIT(31) | 1;
    REG(TM_CONF(t)) = BIT(23);                              /* hold in reset while configuring */
    REG(TM_CONF(t)) = (uint32_t)div << 4 | res | BIT(25);
    tm_res[t] = res;
    return (uint64_t)80000000 * 256 * 1000 / (div << res);
}

static void latch(int ch)
{
    REG_SET(CH_CONF1(ch), BIT(31));
    REG_SET(CH_CONF0(ch), BIT(4));
}

int ledc_channel(int ch, int t, int pin, uint32_t duty)
{
    if (ch < 0 || ch >= LEDC_CHANNELS || t < 0 || t >= LEDC_TIMERS) return -1;
    if (gpio_config(pin, GPIO_OUTPUT | GPIO_INPUT) < 0) return -1;   /* input: readback for tests */
    ch_tm[ch] = t;
    REG(CH_HPOINT(ch)) = 0;
    REG(CH_DUTY(ch)) = duty << 4;
    REG(CH_CONF1(ch)) = 0;
    REG(CH_CONF0(ch)) = t | BIT(2);
    latch(ch);
    gpio_out_signal(pin, LEDC_SIG_OUT0 + ch, false);
    return 0;
}

void ledc_stop(int ch, bool idle)
{
    REG(CH_CONF0(ch)) = ch_tm[ch] | (idle ? BIT(3) : 0);
    REG_SET(CH_CONF0(ch), BIT(4));
}

RAM_FN void ledc_duty_phase(int ch, uint32_t duty, uint32_t hpoint)
{
    REG(CH_HPOINT(ch)) = hpoint;
    REG(CH_DUTY(ch)) = duty << 4;
    REG(CH_CONF1(ch)) = BIT(31);
    REG(CH_CONF0(ch)) = ch_tm[ch] | BIT(2) | BIT(4);
}

RAM_FN void ledc_duty(int ch, uint32_t duty)
{
    REG(CH_DUTY(ch)) = duty << 4;
    REG(CH_CONF1(ch)) = BIT(31);
    REG(CH_CONF0(ch)) = ch_tm[ch] | BIT(2) | BIT(4);
}

void ledc_fade(int ch, uint32_t from, bool up, uint32_t steps, uint32_t cycles)
{
    REG(CH_DUTY(ch)) = from << 4;
    REG(CH_CONF1(ch)) = BIT(31) | (up ? BIT(30) : 0) | (steps & 0x3FF) << 20 |
                        (cycles & 0x3FF) << 10 | 1u << 4;    /* scale: 1 duty step (x16) */
    REG(CH_CONF0(ch)) = ch_tm[ch] | BIT(2) | BIT(4);
}
