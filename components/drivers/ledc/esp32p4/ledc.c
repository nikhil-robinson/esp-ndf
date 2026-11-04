/* P4 LEDC, same API as drivers/ledc/ledc.c (ledc.h). The P4 block (ESP-IDF v6.1 hw_ver3 ledc_reg.h, ledc_ll.h) has the S3's
 * channel registers but a different timer register (5-bit resolution, divider at b5), the configuration register at 0x170,
 * and a hardware fade engine: 16 "gamma" ranges per channel in LEDC_GAMMA_RAM, each a linear segment (direction, cycles per
 * step, step size, step count) that the hardware chains by itself. ledc_fade_gamma therefore needs no interrupt: the whole
 * perceptual curve is loaded into the ranges and started once. The source clock is PLL_F80M (80 MHz, as the S3's APB). */
#include "ledc.h"
#include "gpio.h"
#include "clk.h"
#include "cpu.h"
#include "fastmath.h"
#include "soc.h"

#define LEDC_BASE        (HPPERIPH1 + 0x13000)
#define CH_CONF0(c)      (LEDC_BASE + 0x14 * (c) + 0x00)  /* b1:0 timer b2 out_en b3 idle b4 para_up b14:5 ovf_num b15 ovf_cnt_en */
#define CH_HPOINT(c)     (LEDC_BASE + 0x14 * (c) + 0x04)
#define CH_DUTY(c)       (LEDC_BASE + 0x14 * (c) + 0x08)  /* duty << 4 */
#define CH_CONF1(c)      (LEDC_BASE + 0x14 * (c) + 0x0C)  /* b31 duty_start */
#define TM_CONF(t)       (LEDC_BASE + 0xA0 + 8 * (t))     /* b4:0 res b22:5 div(10.8) b23 pause b24 rst b26 para_up */
#define INT_RAW          (LEDC_BASE + 0xC0)
#define INT_ENA          (LEDC_BASE + 0xC8)
#define INT_CLR          (LEDC_BASE + 0xCC)               /* fade end of channel c: bit 4 + c */
#define GAMMA_CONF(c)    (LEDC_BASE + 0x100 + 4 * (c))    /* b4:0 entry number b5 pause b6 resume */
#define LEDC_CONF        (LEDC_BASE + 0x170)              /* b1:0 timer clock (0 = APB) b9:2 gamma RAM clock per channel b31 clk_en */
#define GAMMA_RAM(c, r)  (LEDC_BASE + 0x400 + 0x40 * (c) + 4 * (r))   /* b0 inc b10:1 cycle b20:11 scale b30:21 num */
#define LEDC_SIG_OUT0    126
#define SRC_CLK_HZ       80000000u
#define PERI_CLK_CTRL22  (HP_CLKRST_BASE + 0x9C)          /* b1:0 LEDC source (2 = PLL_F80M) */

static uint8_t tm_res[LEDC_TIMERS];
static uint32_t tm_hz[LEDC_TIMERS];
static uint8_t ch_tm[LEDC_CHANNELS];

uint64_t ledc_timer(int t, uint32_t hz, int res)
{
    if (t < 0 || t >= LEDC_TIMERS || res < 1 || res > 14 || !hz) return 0;
    uint64_t den = (uint64_t)hz << res;
    uint64_t div = ((uint64_t)SRC_CLK_HZ * 256 + den / 2) / den;       /* x256, rounded to nearest */
    if (div < 256 || div > 0x3FFFF) return 0;
    periph_enable(PERIPH_LEDC);
    REG_FIELD(PERI_CLK_CTRL22, 0, 2, 2);
    REG(LEDC_CONF) = BIT(31) | 0x3FC;                                  /* clock on, gamma RAM clocks on, timers from APB */
    REG(TM_CONF(t)) = BIT(24);                                         /* hold in reset while configuring */
    REG(TM_CONF(t)) = (uint32_t)div << 5 | (uint32_t)res | BIT(26);
    tm_res[t] = (uint8_t)res;
    tm_hz[t] = (uint32_t)((uint64_t)SRC_CLK_HZ * 256 / (div << res));
    return (uint64_t)SRC_CLK_HZ * 256 * 1000 / (div << res);
}

static void latch(int ch)
{
    REG_SET(CH_CONF1(ch), BIT(31));
    REG_SET(CH_CONF0(ch), BIT(4));
}

int ledc_channel(int ch, int t, int pin, uint32_t duty)
{
    if (ch < 0 || ch >= LEDC_CHANNELS || t < 0 || t >= LEDC_TIMERS) return -1;
    if (gpio_config(pin, GPIO_OUTPUT | GPIO_INPUT) < 0) return -1;
    ch_tm[ch] = (uint8_t)t;
    REG(CH_HPOINT(ch)) = 0;
    REG(CH_DUTY(ch)) = duty << 4;
    REG(CH_CONF1(ch)) = 0;
    REG(CH_CONF0(ch)) = (uint32_t)t | BIT(2);
    latch(ch);
    gpio_out_signal(pin, LEDC_SIG_OUT0 + ch, false);
    return 0;
}

void ledc_stop(int ch, bool idle)
{
    if (ch < 0 || ch >= LEDC_CHANNELS) return;
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

/* load range r of channel ch: duty goes `steps` times by `scale`, one step every `cyc` PWM periods */
static void range_set(int ch, int r, bool up, uint32_t steps, uint32_t cyc, uint32_t scale)
{
    REG(GAMMA_RAM(ch, r)) = (up ? 1u : 0u) | (cyc & 0x3FF) << 1 | (scale & 0x3FF) << 11 | (steps & 0x3FF) << 21;
}

void ledc_fade(int ch, uint32_t from, bool up, uint32_t steps, uint32_t cycles)
{
    if (ch < 0 || ch >= LEDC_CHANNELS) return;
    REG(CH_DUTY(ch)) = from << 4;
    range_set(ch, 0, up, steps, cycles, 1);                             /* scale: duty units (IDF fade_with_step) */
    REG(GAMMA_CONF(ch)) = 1;                                            /* one range */
    REG(INT_CLR) = BIT(4 + ch);
    REG(CH_CONF1(ch)) = BIT(31);
    REG(CH_CONF0(ch)) = ch_tm[ch] | BIT(2) | BIT(4);
}

/* ---- gamma fade: 16 hardware ranges chained by the engine; duty = max * b^gamma over a linear brightness b ---- */
#define SEGS 16
static volatile bool busy[LEDC_CHANNELS];

int ledc_fade_gamma(int ch, uint32_t from, uint32_t to, uint32_t ms, uint32_t gamma_x100)
{
    if (ch < 0 || ch >= LEDC_CHANNELS || !ms || !gamma_x100) return -1;
    if (ledc_fade_busy(ch)) return -1;
    float g = gamma_x100 / 100.0f, max = (float)(1u << tm_res[ch_tm[ch]]);
    float b0 = from ? fm_expf(fm_logf(from / max) / g) : 0, b1 = to ? fm_expf(fm_logf(to / max) / g) : 0;
    uint32_t periods = (uint32_t)((uint64_t)tm_hz[ch_tm[ch]] * ms / 1000), per = periods / SEGS ? periods / SEGS : 1;
    uint32_t cur = from;
    for (int i = 0; i < SEGS; i++) {
        float b = b0 + (b1 - b0) * (i + 1) / SEGS;
        uint32_t nxt = b > 0 ? (uint32_t)(max * fm_expf(g * fm_logf(b)) + 0.5f) : 0;
        if (nxt > (uint32_t)max) nxt = (uint32_t)max;
        if (i == SEGS - 1) nxt = to;
        bool up = nxt >= cur;
        uint32_t delta = up ? nxt - cur : cur - nxt, steps, cyc, scale;
        if (delta >= per) { cyc = 1; steps = per > 1023 ? 1023 : per; scale = (delta + steps / 2) / steps; }
        else { steps = delta ? delta : 1; scale = delta ? 1 : 0; cyc = (per + steps / 2) / steps; }
        if (scale > 1023) scale = 1023;
        if (cyc > 1023) cyc = 1023;
        range_set(ch, i, up, steps, cyc, scale);
        uint32_t end = up ? cur + steps * scale : (cur > steps * scale ? cur - steps * scale : 0);
        cur = end > (uint32_t)max ? (uint32_t)max : end;                 /* the next range starts where this one ends */
    }
    REG(CH_DUTY(ch)) = from << 4;
    REG(GAMMA_CONF(ch)) = SEGS;
    REG(INT_CLR) = BIT(4 + ch);
    busy[ch] = true;
    REG(CH_CONF1(ch)) = BIT(31);
    REG(CH_CONF0(ch)) = ch_tm[ch] | BIT(2) | BIT(4);
    return 0;
}

bool ledc_fade_busy(int ch)
{
    if (ch < 0 || ch >= LEDC_CHANNELS) return false;
    if (busy[ch] && (REG(INT_RAW) & BIT(4 + ch))) busy[ch] = false;     /* fade end flag (all ranges done) */
    return busy[ch];
}
