/* PCNT driver with gap-free 32-bit counts: total = wraps * PCNT_WRAP + hw.
 * The wrap IRQ counts whole wraps from the latched limit status; a read that
 * finds a wrap flagged but not yet serviced accounts for it first, then
 * re-reads the counter, so wraps and the hardware count always agree. */
#include "pcnt.h"
#include "gpio.h"
#include "clk.h"
#include "irq.h"
#include "xtensa.h"

#define PCNT_BASE      0x60017000
#define U_CONF0(u)     (PCNT_BASE + 0x0C * (u) + 0x0)   /* b9:0 filter b10 filt_en b12 h_lim b13 l_lim, ch0 b23:16, ch1 b31:24 */
#define U_CONF2(u)     (PCNT_BASE + 0x0C * (u) + 0x8)   /* b15:0 h_lim b31:16 l_lim */
#define U_CNT(u)       (PCNT_BASE + 0x30 + 4 * (u))
#define INT_RAW        (PCNT_BASE + 0x40)
#define INT_ST         (PCNT_BASE + 0x44)
#define INT_ENA        (PCNT_BASE + 0x48)
#define INT_CLR        (PCNT_BASE + 0x4C)
#define U_STATUS(u)    (PCNT_BASE + 0x50 + 4 * (u))     /* b4 l_lim_lat b5 h_lim_lat */
#define CTRL           (PCNT_BASE + 0x60)               /* b2u rst b2u+1 pause b16 clk_en */
#define SRC_PCNT       41

/* input signals: unit u, channel c: sig = 33 + 4u + c, ctrl = 35 + 4u + c */
#define SIG_IN(u, c)   (33 + 4 * (u) + (c))
#define CTRL_IN(u, c)  (35 + 4 * (u) + (c))

static int32_t wraps[PCNT_UNITS];
static int cpu_int = -1;

static void account(int u)                  /* consume one flagged limit event */
{
    uint32_t st = REG(U_STATUS(u));
    REG(INT_CLR) = BIT(u);
    if (st & BIT(5)) wraps[u]++;            /* h_lim_lat */
    if (st & BIT(4)) wraps[u]--;            /* l_lim_lat */
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
    REG(INT_CLR) = BIT(u);
    REG_SET(INT_ENA, BIT(u));
    pcnt_clear(u);
}

int pcnt_channel(int u, int c, int sig, int ctl, pcnt_edge_t pos, pcnt_edge_t neg,
                 pcnt_ctrl_t hi, pcnt_ctrl_t lo)
{
    if (u < 0 || u >= PCNT_UNITS || c < 0 || c > 1) return -1;
    unit_init(u);
    uint32_t mode = neg | pos << 2 | hi << 4 | lo << 6;
    REG_FIELD(U_CONF0(u), 16 + 8 * c, 8, mode);
    gpio_in_signal(sig, SIG_IN(u, c), false);
    gpio_in_signal(ctl < 0 ? 0x38 : ctl, CTRL_IN(u, c), false);   /* 0x38 = constant 1 */
    return 0;
}

int pcnt_quadrature(int u, int a, int b)
{
    /* x4: ch0 counts A edges qualified by B, ch1 counts B edges qualified by A */
    if (pcnt_channel(u, 0, a, b, PCNT_EDGE_UP, PCNT_EDGE_DOWN, PCNT_CTRL_INVERT, PCNT_CTRL_KEEP)) return -1;
    return pcnt_channel(u, 1, b, a, PCNT_EDGE_UP, PCNT_EDGE_DOWN, PCNT_CTRL_KEEP, PCNT_CTRL_INVERT);
}

void pcnt_filter(int u, uint32_t ns)
{
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
    int32_t v = wraps[u] * PCNT_WRAP + r;
    irq_unlock(ps);
    return v;
}

void pcnt_clear(int u)
{
    uint32_t ps = irq_lock();
    REG_SET(CTRL, BIT(2 * u));
    REG_CLR(CTRL, BIT(2 * u));
    wraps[u] = 0;
    REG(INT_CLR) = BIT(u);
    irq_unlock(ps);
}

void pcnt_pause(int u, bool p) { REG_FIELD(CTRL, 2 * u + 1, 1, p); }
