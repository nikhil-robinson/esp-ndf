/* MCPWM driver. Register offsets per TRM ch. 36. */
#include "mcpwm.h"
#include "gpio.h"
#include "clk.h"

#define U_BASE(u)        ((u) ? 0x6002C000 : 0x6001E000)
#define CLK_CFG(u)       (U_BASE(u) + 0x000)              /* b7:0 group prescale (160 MHz / (n+1)) */
#define T_CFG0(u, t)     (U_BASE(u) + 0x004 + 0x10 * (t)) /* b7:0 prescale b23:8 period b25:24 upmethod */
#define T_CFG1(u, t)     (U_BASE(u) + 0x008 + 0x10 * (t)) /* b2:0 start b4:3 mode */
#define T_SYNC(u, t)     (U_BASE(u) + 0x00C + 0x10 * (t)) /* b0 synci_en b1 sw b3:2 synco b19:4 phase */
#define SYNCI_CFG(u)     (U_BASE(u) + 0x034)              /* timer t syncisel at 3*t */
#define OP_TIMERSEL(u)   (U_BASE(u) + 0x038)
#define G_STMP_CFG(u, o) (U_BASE(u) + 0x03C + 0x38 * (o))
#define G_TSTMP(u, o, k) (U_BASE(u) + 0x040 + 0x38 * (o) + 4 * (k))
#define G_CFG0(u, o)     (U_BASE(u) + 0x048 + 0x38 * (o))
#define G_FORCE(u, o)    (U_BASE(u) + 0x04C + 0x38 * (o))
#define G_ACT(u, o, k)   (U_BASE(u) + 0x050 + 0x38 * (o) + 4 * (k))
#define DT_CFG(u, o)     (U_BASE(u) + 0x058 + 0x38 * (o))
#define DT_FED(u, o)     (U_BASE(u) + 0x05C + 0x38 * (o))
#define DT_RED(u, o)     (U_BASE(u) + 0x060 + 0x38 * (o))
#define CARRIER(u, o)    (U_BASE(u) + 0x064 + 0x38 * (o))
#define FH_CFG0(u, o)    (U_BASE(u) + 0x068 + 0x38 * (o))  /* b4 sw one-shot b23:8 A/B actions */
#define FH_CFG1(u, o)    (U_BASE(u) + 0x06C + 0x38 * (o))  /* b0 clr_ost (rising edge) b4 force_ost (toggle) */
#define CAP_TIMER_CFG(u) (U_BASE(u) + 0x0E8)
#define CAP_CFG(u, c)    (U_BASE(u) + 0x0F0 + 4 * (c))    /* b0 en b2:1 mode(neg,pos) b10:3 prescale */
#define CAP_VAL(u, c)    (U_BASE(u) + 0x0FC + 4 * (c))
#define CAP_STATUS(u)    (U_BASE(u) + 0x108)              /* bit c: 1 = last edge was negative */
#define UPDATE_CFG(u)    (U_BASE(u) + 0x10C)
#define INT_ENA(u)       (U_BASE(u) + 0x110)
#define INT_ST(u)        (U_BASE(u) + 0x118)
#define INT_CLR(u)       (U_BASE(u) + 0x11C)
#define REGCLK(u)        (U_BASE(u) + 0x120)

#define SIG_OUT0A(u)     ((u) ? 166 : 160)                /* +2*op (+1 for B) */
#define SIG_CAP0(u)      ((u) ? 175 : 166)

/* generator actions: 0 none, 1 low, 2 high, 3 toggle */
#define ACT(ev, a)       ((uint32_t)(a) << (2 * (ev)))
enum { UTEZ, UTEP, UTEA, UTEB, UT0, UT1, DTEZ, DTEP, DTEA, DTEB };
enum { NONE, LOW, HIGH };

typedef struct {
    bool up[2];
    uint8_t mode[3];
    irq_fn_t tez_fn[3]; void *tez_arg[3];
    mcpwm_cap_fn_t cap_fn[3]; void *cap_arg[3];
    int cpu_int;
} unit_t;
static unit_t unit[2] = { { .cpu_int = -1 }, { .cpu_int = -1 } };

static void unit_up(int u)
{
    if (unit[u].up[0]) return;
    periph_enable(u ? PERIPH_MCPWM1 : PERIPH_MCPWM0);
    REG(REGCLK(u)) = 1;
    REG(CLK_CFG(u)) = 0;                       /* group: 160 MHz, finest */
    REG(UPDATE_CFG(u)) = 0x55;                 /* b0 global, b2/b4/b6 op0-2 shadow update enable */
    unit[u].up[0] = true;
}

RAM_FN static void unit_isr(void *arg)
{
    int u = (int)arg;
    uint32_t st = REG(INT_ST(u));
    REG(INT_CLR(u)) = st;
    for (int t = 0; t < 3; t++)
        if ((st & BIT(3 + t)) && unit[u].tez_fn[t]) unit[u].tez_fn[t](unit[u].tez_arg[t]);
    for (int c = 0; c < 3; c++)
        if ((st & BIT(27 + c)) && unit[u].cap_fn[c]) {
            uint32_t v = REG(CAP_VAL(u, c));
            int edge = (REG(CAP_STATUS(u)) >> c) & 1 ? MCPWM_NEG : MCPWM_POS;
            unit[u].cap_fn[c](c, v, edge, unit[u].cap_arg[c]);
        }
}

static int unit_irq(int u, int level)
{
    if (unit[u].cpu_int < 0) unit[u].cpu_int = irq_attach(u ? 32 : 31, level, unit_isr, (void *)u);
    return unit[u].cpu_int;
}

uint32_t mcpwm_timer(int u, int t, uint32_t hz, mcpwm_mode_t mode, uint32_t *tick_hz)
{
    if (u < 0 || u > 1 || t < 0 || t > 2 || !hz) return 0;
    unit_up(u);
    /* ticks per period at 160 MHz; up-down counts twice per period */
    uint32_t div = mode == MCPWM_UPDOWN ? 2 : 1;
    for (uint32_t p = 1; p <= 256; p++) {
        uint64_t ticks = 160000000ull / p;
        if (ticks % ((uint64_t)hz * div)) continue;             /* exact frequencies only */
        uint32_t per = ticks / ((uint64_t)hz * div);
        if (per > 65535 || per < 2) continue;
        REG(T_CFG1(u, t)) = 0;                                   /* stop */
        uint32_t reg_per = mode == MCPWM_UP ? per - 1 : per;    /* UP counts 0..per-1 */
        REG(T_CFG0(u, t)) = (p - 1) | reg_per << 8 | 1u << 24;  /* period shadowed at TEZ */
        REG(T_CFG1(u, t)) = (uint32_t)mode << 3;
        unit[u].mode[t] = mode;
        if (tick_hz) *tick_hz = 160000000 / p;
        return per;
    }
    return 0;
}

void mcpwm_start(int u, int t) { REG_FIELD(T_CFG1(u, t), 0, 3, 2); }   /* run free */
void mcpwm_stop(int u, int t)  { REG_FIELD(T_CFG1(u, t), 0, 3, 0); }   /* stop at zero */

void mcpwm_sync(int u, int t, int master, uint32_t phase)
{
    REG_FIELD(T_SYNC(u, master), 2, 2, 1);                 /* master sync-out at TEZ */
    REG_FIELD(SYNCI_CFG(u), 3 * t, 3, master + 1);         /* t listens to master's sync-out */
    REG(T_SYNC(u, t)) = BIT(0) | 3u << 2 | (phase & 0xFFFF) << 4;   /* synci_en, synco none */
}

int mcpwm_on_period(int u, int t, irq_fn_t fn, void *arg, int level)
{
    unit[u].tez_fn[t] = fn; unit[u].tez_arg[t] = arg;
    if (unit_irq(u, level) < 0) return -1;
    REG(INT_CLR(u)) = BIT(3 + t);
    REG_SET(INT_ENA(u), BIT(3 + t));
    return 0;
}

static void op_bind(int u, int op, int t)
{
    unit_up(u);
    REG_FIELD(OP_TIMERSEL(u), 2 * op, 2, t);
    REG(G_STMP_CFG(u, op)) = 1 | 1 << 4;                   /* A, B compares load at TEZ */
    REG(G_CFG0(u, op)) = 0;
    REG(G_FORCE(u, op)) = 0;
    REG(CARRIER(u, op)) = 0;
    REG(FH_CFG0(u, op)) = 0;
}

/* high while count < cmp, for both counting modes */
static uint32_t act_for(int u, int op, int ev_cmp_up, int ev_cmp_dn)
{
    int t = (REG(OP_TIMERSEL(u)) >> (2 * op)) & 3;
    if (unit[u].mode[t] == MCPWM_UPDOWN)
        return ACT(ev_cmp_up, LOW) | ACT(ev_cmp_dn, HIGH);
    return ACT(UTEZ, HIGH) | ACT(ev_cmp_up, LOW);
}

int mcpwm_pwm(int u, int op, int t, int pin_a, int pin_b)
{
    op_bind(u, op, t);
    REG(G_ACT(u, op, 0)) = act_for(u, op, UTEA, DTEA);
    REG(G_ACT(u, op, 1)) = act_for(u, op, UTEB, DTEB);
    REG(DT_CFG(u, op)) = BIT(15) | BIT(16);                 /* dead-time bypassed */
    REG(G_TSTMP(u, op, 0)) = 0;
    REG(G_TSTMP(u, op, 1)) = 0;
    if (gpio_config(pin_a, GPIO_OUTPUT | GPIO_INPUT) < 0) return -1;
    gpio_out_signal(pin_a, SIG_OUT0A(u) + 2 * op, false);
    if (pin_b >= 0) {
        if (gpio_config(pin_b, GPIO_OUTPUT | GPIO_INPUT) < 0) return -1;
        gpio_out_signal(pin_b, SIG_OUT0A(u) + 2 * op + 1, false);
    }
    return 0;
}

int mcpwm_complementary(int u, int op, int t, int hi, int lo, uint32_t red, uint32_t fed)
{
    if (mcpwm_pwm(u, op, t, hi, lo) < 0) return -1;
    REG(DT_RED(u, op)) = red & 0xFFFF;
    REG(DT_FED(u, op)) = fed & 0xFFFF;
    /* both delay stages fed from A; FED path inverted -> B = !A with dead band */
    REG(DT_CFG(u, op)) = BIT(14);                           /* fed_outinvert, no bypass */
    return 0;
}

/* Brake through the fault handler: it sits after dead-time, so it forces
 * the real pins (a generator force would come out inverted on B). */
void mcpwm_force(int u, int op, int level)
{
    if (level < 0) {                                       /* release: rising edge on clr_ost */
        REG_CLR(FH_CFG1(u, op), BIT(0));
        REG_SET(FH_CFG1(u, op), BIT(0));
        REG_CLR(FH_CFG1(u, op), BIT(0));
        return;
    }
    uint32_t a = level ? 2 : 1;                            /* 1 force low, 2 force high */
    REG(FH_CFG0(u, op)) = BIT(4) | a << 12 | a << 14 | a << 20 | a << 22;
    REG(FH_CFG1(u, op)) ^= BIT(4);                         /* toggle = trip now */
}

int mcpwm_capture(int u, int ch, int pin, mcpwm_edge_t e, mcpwm_cap_fn_t fn, void *arg, int level)
{
    unit_up(u);
    REG(CAP_TIMER_CFG(u)) = BIT(0);                         /* capture timer on (APB) */
    gpio_in_signal(pin, SIG_CAP0(u) + ch, false);       /* enables pad input; output untouched */
    /* mode bit1 = negedge, bit2 = posedge */
    REG(CAP_CFG(u, ch)) = BIT(0) | ((e & MCPWM_NEG) ? BIT(1) : 0) | ((e & MCPWM_POS) ? BIT(2) : 0);
    REG_CLR(INT_ENA(u), BIT(27 + ch));
    unit[u].cap_fn[ch] = fn; unit[u].cap_arg[ch] = arg;
    if (fn) {
        if (unit_irq(u, level) < 0) return -1;
        REG(INT_CLR(u)) = BIT(27 + ch);
        REG_SET(INT_ENA(u), BIT(27 + ch));
    }
    return 0;
}
