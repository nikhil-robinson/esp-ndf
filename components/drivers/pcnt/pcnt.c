/* PCNT driver with gap-free 32-bit counts: total = wraps * PCNT_WRAP + hw.
 * The wrap IRQ counts whole wraps from the latched limit status; a read that
 * finds a wrap flagged but not yet serviced accounts for it first, then
 * re-reads the counter, so wraps and the hardware count always agree. */
#include "pcnt.h"
#include "gpio.h"
#include "clk.h"
#include "irq.h"
#include "cpu.h"

#if SOC_CPU_RISCV                      /* ESP32-P4: same PCNT block (4 units, 2 channels), other base / source / matrix signals */
#define PCNT_BASE      0x500C9000
#else
#define PCNT_BASE      0x60017000
#endif
#define U_CONF0(u)     (PCNT_BASE + 0x0C * (u) + 0x0)   /* b9:0 filter b10 filt_en b12 h_lim b13 l_lim, ch0 b23:16, ch1 b31:24 */
#define U_CONF1(u)     (PCNT_BASE + 0x0C * (u) + 0x4)   /* b15:0 thres0 b31:16 thres1 */
#define U_CONF2(u)     (PCNT_BASE + 0x0C * (u) + 0x8)   /* b15:0 h_lim b31:16 l_lim */
#define U_CNT(u)       (PCNT_BASE + 0x30 + 4 * (u))
#define INT_RAW        (PCNT_BASE + 0x40)
#define INT_ST         (PCNT_BASE + 0x44)
#define INT_ENA        (PCNT_BASE + 0x48)
#define INT_CLR        (PCNT_BASE + 0x4C)
#define U_STATUS(u)    (PCNT_BASE + 0x50 + 4 * (u))     /* b2 thres1 b3 thres0 b4 l_lim b5 h_lim b6 zero (latched) */
#define CTRL           (PCNT_BASE + 0x60)               /* b2u rst b2u+1 pause b16 clk_en */
#if SOC_CPU_RISCV
#include "soc.h"                        /* SRC_PCNT */
/* input signals (gpio_sig_map.h): PCNT_SIG_CH<c>_PAD_IN<u> = 141 + 4c + u, PCNT_CTRL_CH<c>_PAD_IN<u> = 149 + 4c + u */
#define SIG_IN(u, c)   (141 + 4 * (c) + (u))
#define CTRL_IN(u, c)  (149 + 4 * (c) + (u))
#else
#define SRC_PCNT       41
/* input signals: unit u, channel c: sig = 33 + 4u + c, ctrl = 35 + 4u + c */
#define SIG_IN(u, c)   (33 + 4 * (u) + (c))
#define CTRL_IN(u, c)  (35 + 4 * (u) + (c))
#endif

static int32_t wraps[PCNT_UNITS], off[PCNT_UNITS], lim[PCNT_UNITS] = { PCNT_WRAP, PCNT_WRAP, PCNT_WRAP, PCNT_WRAP };
static int cpu_int = -1;
static struct { pcnt_watch_fn_t fn; void *arg; uint8_t ev; bool step; int32_t idx_at; } w[PCNT_UNITS];

static void account(int u)                  /* consume one flagged event */
{
    uint32_t st = REG(U_STATUS(u));
    REG(INT_CLR) = BIT(u);
    if (st & BIT(5)) wraps[u]++;            /* h_lim_lat */
    if (st & BIT(4)) wraps[u]--;            /* l_lim_lat */
    if (!w[u].fn) return;
    uint32_t ev = (st & BIT(3) ? PCNT_EV_THRES0 : 0) | (st & BIT(2) ? PCNT_EV_THRES1 : 0) |
                  (st & BIT(6) ? PCNT_EV_ZERO : 0) | (st & (BIT(4) | BIT(5)) ? PCNT_EV_LIMIT : 0);
    int32_t at = off[u] + wraps[u] * lim[u] + (int16_t)REG(U_CNT(u));
    if (w[u].step) {                        /* step = the hardware limit: each wrap is one step */
        if (!(ev & PCNT_EV_LIMIT)) return;
        at = off[u] + wraps[u] * lim[u];
        ev = PCNT_EV_STEP;
    } else if (!(ev &= w[u].ev)) return;
    w[u].fn(u, ev, at, w[u].arg);
}

RAM_FN static void pcnt_isr(void *arg)
{
    (void)arg;
    uint32_t st = REG(INT_ST);
    for (int u = 0; u < PCNT_UNITS; u++) if (st & BIT(u)) account(u);
}

static void unit_init(int u)
{
    periph_enable(PERIPH_PCNT);
    REG_SET(CTRL, BIT(16));
    if (cpu_int < 0) cpu_int = irq_attach(SRC_PCNT, 1, pcnt_isr, 0);
    REG(U_CONF2(u)) = (uint32_t)(uint16_t)PCNT_WRAP | (uint32_t)(uint16_t)-PCNT_WRAP << 16;
    REG_SET(U_CONF0(u), BIT(12) | BIT(13));        /* wrap events at h/l limits */
    REG_FIELD(U_CONF0(u), 0, 11, 0);               /* glitch filter off (reset value drops pulses < 10 APB cycles); IDF default too */
    REG(INT_CLR) = BIT(u);
    REG_SET(INT_ENA, BIT(u));
    pcnt_clear(u);
}

int pcnt_channel(int u, int c, int sig, int ctl, pcnt_edge_t pos, pcnt_edge_t neg,
                 pcnt_ctrl_t hi, pcnt_ctrl_t lo)
{
    if (!(u >= 0 && u < PCNT_UNITS)) return -1;      /* caller index: never past the hardware */
    if (u < 0 || u >= PCNT_UNITS || c < 0 || c > 1) return -1;
    unit_init(u);
    uint32_t mode = neg | pos << 2 | hi << 4 | lo << 6;
    REG_FIELD(U_CONF0(u), 16 + 8 * c, 8, mode);
    gpio_in_signal(sig, SIG_IN(u, c), false);
    gpio_in_signal(ctl < 0 ? GPIO_IN_HIGH : ctl, CTRL_IN(u, c), false);   /* constant 1 */
    return 0;
}

int pcnt_quadrature(int u, int a, int b)
{
    if (!(u >= 0 && u < PCNT_UNITS)) return -1;      /* caller index: never past the hardware */
    /* x4: ch0 counts A edges qualified by B, ch1 counts B edges qualified by A */
    if (pcnt_channel(u, 0, a, b, PCNT_EDGE_UP, PCNT_EDGE_DOWN, PCNT_CTRL_INVERT, PCNT_CTRL_KEEP)) return -1;
    return pcnt_channel(u, 1, b, a, PCNT_EDGE_UP, PCNT_EDGE_DOWN, PCNT_CTRL_KEEP, PCNT_CTRL_INVERT);
}

void pcnt_filter(int u, uint32_t ns)
{
    if (!(u >= 0 && u < PCNT_UNITS)) return;      /* caller index: never past the hardware */
    uint32_t cyc = ns * 80 / 1000;                  /* APB cycles */
    if (cyc > 1023) cyc = 1023;
    REG_FIELD(U_CONF0(u), 0, 11, cyc ? (cyc | BIT(10)) : 0);
}

RAM_FN int32_t pcnt_get(int u)
{
    uint32_t ps = irq_lock();
    int32_t r = (int16_t)REG(U_CNT(u));
    /* The limit flag becomes visible ~22 CPU cycles after the counter resets
     * (measured). Near zero a wrap may be in flight: let the flag settle. */
    if ((uint32_t)(r + 256) < 512) { uint32_t t = ccount(); while (ccount() - t < 48) {} }
    if (REG(INT_RAW) & BIT(u)) {            /* wrapped, IRQ not run yet */
        account(u);
        r = (int16_t)REG(U_CNT(u));
    }
    int32_t v = off[u] + wraps[u] * lim[u] + r;
    irq_unlock(ps);
    return v;
}

void pcnt_clear(int u)
{
    uint32_t ps = irq_lock();
    REG_SET(CTRL, BIT(2 * u));
    REG_CLR(CTRL, BIT(2 * u));
    wraps[u] = 0;
    off[u] = 0;
    REG(INT_CLR) = BIT(u);
    irq_unlock(ps);
}

/* thresholds and limits only latch on a counter reset (IDF pulse_cnt.c): keep the running total in
 * off[], then reset. Edges in the few bus cycles between the read and the reset are not counted. */
static void rearm(int u, uint32_t conf1, int32_t limit)
{
    int32_t r = (int16_t)REG(U_CNT(u));
    if (REG(INT_RAW) & BIT(u)) { account(u); r = (int16_t)REG(U_CNT(u)); }
    REG(U_CONF1(u)) = conf1;
    REG(U_CONF2(u)) = (uint32_t)(uint16_t)limit | (uint32_t)(uint16_t)-limit << 16;
    REG_SET(CTRL, BIT(2 * u));
    REG_CLR(CTRL, BIT(2 * u));
    off[u] += wraps[u] * lim[u] + r;
    wraps[u] = 0; lim[u] = limit;
    REG(INT_CLR) = BIT(u);
}

void pcnt_pause(int u, bool p) {
    if (!(u >= 0 && u < PCNT_UNITS)) return;      /* caller index: never past the hardware */
    REG_FIELD(CTRL, 2 * u + 1, 1, p); }

int pcnt_watch(int u, int16_t thres0, int16_t thres1, uint32_t events, pcnt_watch_fn_t fn, void *arg)
{
    if (!(u >= 0 && u < PCNT_UNITS)) return -1;      /* caller index: never past the hardware */
    if (u < 0 || u >= PCNT_UNITS) return -1;
    uint32_t ps = irq_lock();
    w[u].fn = fn; w[u].arg = arg; w[u].ev = events; w[u].step = false;
    REG_FIELD(U_CONF0(u), 11, 1, !!(events & PCNT_EV_ZERO));
    REG_FIELD(U_CONF0(u), 14, 1, !!(events & PCNT_EV_THRES0));
    REG_FIELD(U_CONF0(u), 15, 1, !!(events & PCNT_EV_THRES1));
    rearm(u, (uint32_t)(uint16_t)thres0 | (uint32_t)(uint16_t)thres1 << 16, PCNT_WRAP);
    irq_unlock(ps);
    return 0;
}

int pcnt_step_notify(int u, int32_t step, pcnt_watch_fn_t fn, void *arg)
{
    if (!(u >= 0 && u < PCNT_UNITS)) return -1;      /* caller index: never past the hardware */
    /* no hardware step notify on the S3 (IDF: P4 only): make the wrap limit = step. The counter then
     * wraps by itself at +-step, gap-free, and the existing wrap interrupt is the step event. */
    if (u < 0 || u >= PCNT_UNITS || step <= 0 || step > 32767) return -1;
    uint32_t ps = irq_lock();
    w[u].fn = fn; w[u].arg = arg; w[u].ev = 0; w[u].step = fn != 0;
    REG_CLR(U_CONF0(u), BIT(11) | BIT(14) | BIT(15));
    rearm(u, 0, fn ? step : PCNT_WRAP);
    irq_unlock(ps);
    return 0;
}

static void on_index(void *arg)
{
    int u = (int)arg;
    w[u].idx_at = pcnt_get(u);
    pcnt_clear(u);
}

int pcnt_index(int u, int pin, bool rising)
{
    if (!(u >= 0 && u < PCNT_UNITS)) return -1;      /* caller index: never past the hardware */
    /* no hardware clear input on the S3 (IDF SOC_PCNT_SUPPORT_CLEAR_SIGNAL: not here): GPIO interrupt */
    if (u < 0 || u >= PCNT_UNITS) return -1;
    if (!((pin < 32 ? GPIO_REG(0x20) >> pin : GPIO_REG(0x2C) >> (pin - 32)) & 1))   /* GPIO_ENABLE: */
        gpio_config(pin, GPIO_INPUT);       /* an output pad (shared, loop test) is left as configured */
    return gpio_irq_attach(pin, rising ? GPIO_IRQ_RISING : GPIO_IRQ_FALLING, 3, on_index, (void *)u);
}

int32_t pcnt_index_count(int u) { return w[u].idx_at; }
