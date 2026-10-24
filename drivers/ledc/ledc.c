/* LEDC (low-speed group; the S3 has no high-speed group). */
#include "ledc.h"
#include "gpio.h"
#include "clk.h"
#include "irq.h"
#include "fastmath.h"

#define LEDC_BASE        0x60019000
#define CH_CONF0(c)      (LEDC_BASE + 0x14 * (c) + 0x00)  /* b1:0 timer b2 out_en b3 idle b4 para_up */
#define CH_HPOINT(c)     (LEDC_BASE + 0x14 * (c) + 0x04)
#define CH_DUTY(c)       (LEDC_BASE + 0x14 * (c) + 0x08)  /* duty << 4 (4 dither bits) */
#define CH_CONF1(c)      (LEDC_BASE + 0x14 * (c) + 0x0C)  /* b31 start b30 inc b29:20 num b19:10 cycle b9:0 scale */
#define TM_CONF(t)       (LEDC_BASE + 0xA0 + 8 * (t))     /* b3:0 res b21:4 div(10.8) b22 pause b23 rst b25 para_up */
#define LEDC_CONF        (LEDC_BASE + 0xD0)               /* b1:0 clk sel (1 = APB) b31 clk_en */
#define LEDC_SIG_OUT0    73
#define INT_ST           (LEDC_BASE + 0xC4)
#define INT_ENA          (LEDC_BASE + 0xC8)
#define INT_CLR          (LEDC_BASE + 0xCC)               /* fade end of ch c: bit 4 + c */
#define SRC_LEDC         35

static uint8_t tm_res[LEDC_TIMERS];
static uint32_t tm_hz[LEDC_TIMERS];
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
    tm_hz[t] = (uint32_t)((uint64_t)80000000 * 256 / (div << res));
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
                        (cycles & 0x3FF) << 10 | 1;          /* scale: duty units (IDF fade_with_step) */
    REG(CH_CONF0(ch)) = ch_tm[ch] | BIT(2) | BIT(4);
}

/* ---- gamma fade: the S3 fades linearly only (hardware gamma is C6+): a chain of 16 hardware linear
 * segments, perceptually linear (duty = max * b^gamma), each started from the previous one's end IRQ */
#define SEGS 16
static struct { uint16_t d[SEGS + 1]; uint32_t per; uint8_t i; volatile bool busy; } gf[LEDC_CHANNELS];
static int ledc_int = -1;

RAM_FN static void seg_start(int ch)
{
    /* one segment ~ `per` PWM periods; the step size (scale) is in whole duty units */
    uint32_t cur = gf[ch].d[gf[ch].i], nxt = gf[ch].d[gf[ch].i + 1], up = nxt >= cur;
    uint32_t delta = up ? nxt - cur : cur - nxt, per = gf[ch].per, steps, cyc, scale;
    if (delta >= per) { cyc = 1; steps = per > 1023 ? 1023 : per; scale = (delta + steps / 2) / steps; }
    else { steps = delta ? delta : 1; scale = delta ? 1 : 0; cyc = (per + steps / 2) / steps; }
    if (scale > 1023) scale = 1023;
    if (cyc > 1023) cyc = 1023;
    uint32_t end = up ? cur + steps * scale : (cur > steps * scale ? cur - steps * scale : 0);
    uint32_t max = 1u << tm_res[ch_tm[ch]];
    gf[ch].d[gf[ch].i + 1] = end > max ? max : end;         /* exact end of this segment */
    REG(CH_DUTY(ch)) = cur << 4;
    REG(CH_CONF1(ch)) = BIT(31) | (up ? BIT(30) : 0) | steps << 20 | cyc << 10 | scale;
    REG(CH_CONF0(ch)) = ch_tm[ch] | BIT(2) | BIT(4);
}

RAM_FN static void ledc_isr(void *arg)
{
    (void)arg;
    uint32_t st = REG(INT_ST);
    REG(INT_CLR) = st;
    for (int ch = 0; ch < LEDC_CHANNELS; ch++) {
        if (!(st & BIT(4 + ch)) || !gf[ch].busy) continue;
        if (++gf[ch].i < SEGS) seg_start(ch);
        else { REG_CLR(INT_ENA, BIT(4 + ch)); ledc_duty(ch, gf[ch].d[SEGS]); gf[ch].busy = false; }
    }
}

int ledc_fade_gamma(int ch, uint32_t from, uint32_t to, uint32_t ms, uint32_t gamma_x100)
{
    if (ch < 0 || ch >= LEDC_CHANNELS || gf[ch].busy || !ms || !gamma_x100) return -1;
    if (ledc_int < 0 && (ledc_int = irq_attach(SRC_LEDC, 1, ledc_isr, 0)) < 0) return -1;
    float g = gamma_x100 / 100.0f, max = (float)(1u << tm_res[ch_tm[ch]]);
    float b0 = from ? fm_expf(fm_logf(from / max) / g) : 0, b1 = to ? fm_expf(fm_logf(to / max) / g) : 0;
    for (int i = 0; i <= SEGS; i++) {
        float b = b0 + (b1 - b0) * i / SEGS;
        uint32_t d = b > 0 ? (uint32_t)(max * fm_expf(g * fm_logf(b)) + 0.5f) : 0;
        gf[ch].d[i] = d > (uint32_t)max ? (uint32_t)max : d;
    }
    gf[ch].d[0] = from; gf[ch].d[SEGS] = to;
    uint32_t periods = (uint32_t)((uint64_t)tm_hz[ch_tm[ch]] * ms / 1000);
    gf[ch].per = periods / SEGS ? periods / SEGS : 1;
    gf[ch].i = 0; gf[ch].busy = true;
    REG(INT_CLR) = BIT(4 + ch);
    REG_SET(INT_ENA, BIT(4 + ch));
    seg_start(ch);
    return 0;
}

bool ledc_fade_busy(int ch) { return gf[ch].busy; }
