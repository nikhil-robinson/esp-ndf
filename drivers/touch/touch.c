/* Touch sensor v2 (TRM ch. 39): FSM-timer scanning, same defaults as ESP-IDF. */
#include "touch.h"
static int dn_grade = -1, dn_cap;                          /* touch_denoise(), applied by touch_init */
#include "sys.h"
#include "rtc_irq.h"
#include "xtensa.h"

#define CTRL1      0x60008108   /* b15:0 sleep cycles b31:16 meas num */
#define CTRL2      0x6000810C   /* b3:2 drange b5:4 drefl b7:6 drefh b13 slp_timer b16 start_force b24:17 xpd_wait b28:27 force_done b29 reset b31 clkgate */
#define SCAN_CTRL  0x60008110   /* b2 denoise b8 inactive conn b9 shield b24:10 pad map */
#define DAC        0x6000814C   /* slope pad n (<10) at 29-3n */
#define DAC1       0x60008150   /* slope pad n-10 */
#define TOUCH_CONF 0x6000885C   /* b14:0 outen b17:16 data_sel */
#define STATUS(n)  (0x600088A4 + 4 * ((n) - 1))
#define PAD(n)     (0x60008484 + 4 * (n))   /* b19 mux_sel (RTC) b20 xpd b21 tie b13 fun_ie b27 rue b28 rde */
#define APPROACH   0x60008118   /* b31:24 proximity measure times */
#define FILTER     0x6000811C   /* b7 byp neg noise b8 byp noise b10:9 smooth b14:11 jitter b18:15 cfg1 b20:19 cfg2
                                   b22:21 noise b24:23 cfg3 b27:25 debounce b30:28 mode b31 en */
#define THRES(n)   (0x60008864 + 4 * ((n) - 1))
#define CHN_ST     0x6000889C   /* b14:0 active pads b29:15 channel clear */
#define APPR_ST    0x600088E0   /* proximity counts: pad0 b23:16 pad1 b15:8 pad2 b7:0 */
#define T_ACTIVE   BIT(7)
#define T_INACTIVE BIT(8)

int touch_init(uint16_t mask)
{
    mask &= 0x7FFE;                                       /* pads 1..14 */
    REG_FIELD(CTRL2, 27, 2, 3); REG_FIELD(CTRL2, 13, 1, 0);   /* stop FSM */
    REG_SET(CTRL2, BIT(29)); REG_CLR(CTRL2, BIT(29));        /* reset */
    REG_FIELD(CTRL1, 16, 16, 500);                         /* charge/discharge cycles */
    REG_FIELD(CTRL1, 0, 16, 0xF);                          /* sleep cycles */
    REG_FIELD(CTRL2, 17, 8, 0xFF);                         /* xpd wait */
    REG_FIELD(CTRL2, 6, 2, 3);                             /* drefh 2.7 V */
    REG_FIELD(CTRL2, 4, 2, 0);                             /* drefl 0.5 V */
    REG_FIELD(CTRL2, 2, 2, 2);                             /* drange 0.5 V */
    REG_FIELD(SCAN_CTRL, 8, 1, 1);                         /* idle pads to GND */
    REG_CLR(SCAN_CTRL, BIT(2) | BIT(9));                   /* no shield; denoise as set by touch_denoise() */
    if (dn_grade >= 0) {                                   /* before the FSM starts (IDF: it stalls if changed while scanning) */
        REG_FIELD(DAC, 29, 3, 7);                          /* pad 0 = denoise channel, IDF default slope */
        REG_FIELD(CTRL2, 9, 3, dn_cap & 7);
        REG_FIELD(SCAN_CTRL, 0, 2, dn_grade & 3);
        REG_SET(SCAN_CTRL, BIT(2));
    }
    REG_SET(CTRL2, BIT(31));                               /* clock */
    for (int n = 1; n <= 14; n++) {
        if (!(mask & BIT(n))) continue;
        REG(PAD(n)) = BIT(19);                             /* RTC mux, analog: no IE, no pulls, float */
        if (n < 10) REG_FIELD(DAC, 29 - 3 * n, 3, 7);
        else        REG_FIELD(DAC1, 29 - 3 * (n - 10), 3, 7);
    }
    for (int n = 1; n <= 14; n++) REG(THRES(n)) = 0x3FFFFF;     /* no events until touch_threshold() */
    REG_FIELD(SCAN_CTRL, 10, 15, dn_grade >= 0 ? mask & ~1u : mask);   /* scan exactly these pads (not 0 with denoise) */
    REG_FIELD(TOUCH_CONF, 0, 15, mask);
    REG_FIELD(TOUCH_CONF, 16, 2, 0);                       /* read raw data */
    REG_FIELD(CTRL2, 16, 1, 0);                            /* FSM driven by timer */
    REG_FIELD(CTRL2, 27, 2, 0);
    REG_SET(CTRL2, BIT(13));                               /* start the scan timer */
    return 0;
}

uint32_t touch_read(int pad) { return REG(STATUS(pad)) & 0x3FFFFF; }

static uint32_t data_read(int pad, uint32_t sel)            /* TOUCH_CONF data_sel: 0 raw 2 benchmark 3 smooth */
{
    uint32_t ps = irq_lock();
    REG_FIELD(TOUCH_CONF, 16, 2, sel);
    uint32_t v = REG(STATUS(pad)) & 0x3FFFFF;
    REG_FIELD(TOUCH_CONF, 16, 2, 0);
    irq_unlock(ps);
    return v;
}
uint32_t touch_benchmark(int pad) { return data_read(pad, 2); }
uint32_t touch_smooth(int pad)    { return data_read(pad, 3); }

void touch_filter(bool on)
{
    /* IDF touch_pad_filter_set_config defaults: IIR 16, debounce 1, noise 0, jitter step 4, smooth IIR 2 */
    if (!on) { REG_CLR(FILTER, BIT(31)); return; }
    uint32_t v = REG(FILTER) & 0x7F;
    v |= 1u << 9 | 4u << 11 | 0xFu << 15 | 0u << 19 | 0u << 21 | 2u << 23 | 1u << 25 | 2u << 28 | BIT(31);
    REG(FILTER) = v;
    REG(CHN_ST) = 0x7FFEu << 15;                            /* restart the benchmarks from the current value */
}

void touch_threshold(int pad, uint32_t thr) { REG(THRES(pad)) = thr & 0x3FFFFF; }

static touch_fn_t ev_fn; static void *ev_arg; static uint16_t last_active;

RAM_FN static void touch_isr(void *a)
{
    (void)a;
    rtc_irq_clear(T_ACTIVE | T_INACTIVE);
    uint16_t act = REG(CHN_ST) & 0x7FFE, ch = act ^ last_active;
    last_active = act;
    for (int n = 1; n <= 14; n++) if ((ch & BIT(n)) && ev_fn) ev_fn(n, (act >> n) & 1, ev_arg);
}

int touch_on_event(touch_fn_t fn, void *arg)
{
    ev_fn = fn; ev_arg = arg;
    if (!fn) { rtc_irq_off(T_ACTIVE | T_INACTIVE); return 0; }
    last_active = REG(CHN_ST) & 0x7FFE;
    return rtc_irq_on(T_ACTIVE | T_INACTIVE, touch_isr, 0);
}

uint16_t touch_active(void) { return REG(CHN_ST) & 0x7FFE; }

void touch_proximity(const int8_t pads[3], uint32_t meas_times)
{
    /* IDF touch_pad_proximity_enable: the 3 approach channels accumulate meas_times scans */
    uint32_t v = REG(TOUCH_CONF) & ~(0xFFFu << 20);
    for (int i = 0; i < 3; i++) v |= (uint32_t)(pads[i] > 0 ? pads[i] : 0) << (28 - 4 * i);
    REG(TOUCH_CONF) = v;
    REG_FIELD(APPROACH, 24, 8, meas_times);
}

uint32_t touch_proximity_count(int i) { return (REG(APPR_ST) >> (16 - 8 * i)) & 0xFF; }   /* pad0 b23:16 pad1 b15:8 pad2 b7:0 */

/* Denoise (IDF touch_pad_denoise_*): pad 0 is measured against an internal reference capacitor and its count is
 * subtracted from every pad's reading (cancels supply / common-mode noise). Takes effect at the next touch_init. */
void touch_denoise(int grade, int cap) { dn_grade = grade; dn_cap = cap; }

uint32_t touch_denoise_data(void) { return REG(0x60008860) & 0x3FFFFF; }

