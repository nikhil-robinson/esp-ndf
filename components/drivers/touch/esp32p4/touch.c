/* P4 touch sensor, same API as drivers/touch/touch.c (touch.h). The P4 block is the "v3" sensor: channels 1-14 on
 * GPIO2-15 (pad n = GPIO n + 1), an FSM scanning the enabled channels on a timer in the PMU, per-channel benchmark /
 * smooth filters in hardware, thresholds per channel, and the results in LP_TOUCH. Registers are offsets into
 * LP_ANALOG_PERI (0x50113000), LP_TOUCH (0x50128000) and PMU (0x50115000), taken from ESP-IDF v6.1 hw_ver3
 * lp_analog_peri_struct.h / touch_struct.h / pmu_struct.h and esp_hal_touch_sens/esp32p4 touch_sensor_ll.h. One sample
 * configuration (charge times 500, defaults of the IDF driver). */
#include "touch.h"
#include "sys.h"
#include "irq.h"
#include "cpu.h"
#include "soc.h"

#define AP(o)        (LPAON + 0x3000 + (o))            /* LP_ANALOG_PERI */
#define LT(o)        (LPPERIPH + 0x8000 + (o))         /* LP_TOUCH */
#define PMU_TOUCH    (PMU_BASE + 0x210)                /* b30 force_done b31 sleep_timer_en b29:14 sleep_cycles */
#define SCAN1        AP(0x100)                         /* b16:2 scan_pad_map b31:17 xpd_wait */
#define SCAN2        AP(0x104)                         /* b27 freq_scan_en b29:28 freq_scan_cnt_limit b22 timeout_en b21:6 timeout_num */
#define WORK         AP(0x108)                         /* b27 out_gate b25 out_sel b24:22 div0 */
#define MEAS_NUM     AP(0x10C)                         /* b29:20 meas_num0 */
#define FILTER1      AP(0x110)                         /* b2:1 hysteresis b4:3 nn_thres b6:5 noise_thres b8:7 smooth b12:9 jitter
                                                          b15:13 mode b16 filter_en b20:17 nn_limit b28:21 approach_limit b31:29 debounce */
#define FILTER2      AP(0x114)                         /* b29:15 outen b30 bypass_noise b31 bypass_nn */
#define FILTER3      AP(0x118)                         /* b15:0 benchmark_sw b16 update_benchmark_sw */
#define CLR          AP(0x124)                         /* b14:0 channel_clr (benchmark reset) b15 status_clr */
#define APPROACH     AP(0x128)                         /* b3:0 pad0 b7:4 pad1 b11:8 pad2 */
#define FREQ_PARA    AP(0x12C)                         /* b6:0 dcap_lpf b8:7 dres_lpf b12:9 drv_ls b17:13 drv_hs b22:18 dbias */
#define MUX0         AP(0x13C)                         /* b9:8 data_sel b11:10 freq_sel b28 done_force b29 fsm_en b30 start_en b31 start_force */
#define APPR_MEAS    AP(0xFC)                          /* b29:20 approach_meas_num0 */
#define THN(n)       AP(0x14C + 12 * (n))              /* b31:16 threshold, sample config 0 */
#define T_CHN_ST     LT(0x10)                          /* b14:0 pad_active */
#define T_CHN_DATA(n) LT(0x14 + 4 * (n))               /* b15:0 pad_data */
#define T_APPR_DATA  LT(0x54)                          /* b23:16 pad0 b15:8 pad1 b7:0 pad2 */
#define T_INT_ST     LT(0x4)                           /* b2 active b3 inactive (INT_ENA +8, INT_CLR +0xC) */
#define T_INT_ENA    LT(0x8)
#define T_INT_CLR    LT(0xC)
#define LPPERI_CLK   (LPPERIPH + 0x0)                  /* b21 LP_TOUCH clock */
#define LPPERI_RST   (LPPERIPH + 0x8)                  /* LPPERI_RESET_EN, b22 LP_TOUCH */
#define SRC_LP_TOUCH 14
#define T_ACTIVE     BIT(2)
#define T_INACTIVE   BIT(3)

static int dn_grade = -1;

static void force_done(void)                           /* finish the measurement in flight (IDF touch_ll_force_done_curr_measurement) */
{
    REG_SET(PMU_TOUCH, BIT(30));
    delay_us(100);
    REG_CLR(PMU_TOUCH, BIT(30));
}

int touch_init(uint16_t mask)
{
    mask &= 0x7FFE;                                       /* pads 1..14 */
    REG_SET(LPPERI_CLK, BIT(21));
    REG_SET(LPPERI_RST, BIT(22)); REG_CLR(LPPERI_RST, BIT(22));      /* LP_TOUCH reset (LPPERI_RESET_EN b22) */
    REG_CLR(PMU_TOUCH, BIT(31)); force_done();            /* stop the timer */
    REG_FIELD(SCAN1, 17, 15, 0xFF);                       /* power-on wait cycles */
    REG_FIELD(MEAS_NUM, 20, 10, 500);                     /* charge times (sample config 0) */
    REG_FIELD(PMU_TOUCH, 14, 16, 0xF);                    /* measure interval */
    REG_FIELD(WORK, 22, 3, 0);                            /* divider 1 */
    REG_CLR(SCAN2, BIT(27));                              /* one sample configuration */
    REG_FIELD(FREQ_PARA, 0, 7, 0); REG_FIELD(FREQ_PARA, 9, 4, 3); REG_FIELD(FREQ_PARA, 13, 5, 3); REG_FIELD(FREQ_PARA, 18, 5, 0xF);   /* IDF defaults: driver ls 3, hs 3, bias 15 */
    for (int n = 1; n <= 14; n++) if (mask & BIT(n)) {
        int gpio = n + 1;                                 /* analog: no IE, no pulls, no output */
        REG(IO_MUX_BASE + 4 + 4 * gpio) = 0;
    }
    for (int n = 1; n <= 14; n++) REG_FIELD(THN(n), 16, 16, 0xFFFF);  /* no events until touch_threshold() */
    REG_FIELD(SCAN1, 2, 15, mask);                        /* scan exactly these pads */
    REG_FIELD(FILTER2, 15, 15, mask);                     /* outputs enabled */
    if (dn_grade >= 0) {                                  /* IDF touch_filter_set_denoise_level: 0 = always update benchmark */
        bool always = dn_grade == 0;
        uint32_t thr = dn_grade >= 4 ? 3 : 3 - (uint32_t)dn_grade;
        REG_FIELD(FILTER2, 30, 1, always); REG_FIELD(FILTER2, 31, 1, always);
        REG_FIELD(FILTER1, 5, 2, always ? 0 : thr); REG_FIELD(FILTER1, 3, 2, always ? 0 : thr); REG_FIELD(FILTER1, 17, 4, 5);
    }
    REG_FIELD(MUX0, 8, 2, 0);                             /* read raw data */
    REG_SET(MUX0, BIT(29)); REG_CLR(MUX0, BIT(28) | BIT(31));   /* FSM driven by the timer */
    REG_SET(PMU_TOUCH, BIT(31));                          /* start the scan timer */
    return 0;
}

static uint32_t data_read(int pad, uint32_t sel)          /* MUX0 data_sel: 0 raw, 2 benchmark, 3 smooth */
{
    if (pad < 1 || pad > 14) return 0;
    uint32_t ps = irq_lock();
    REG_FIELD(MUX0, 8, 2, sel);
    uint32_t v = REG(T_CHN_DATA(pad)) & 0xFFFF;
    REG_FIELD(MUX0, 8, 2, 0);
    irq_unlock(ps);
    return v;
}
uint32_t touch_read(int pad)      { return data_read(pad, 0); }
uint32_t touch_benchmark(int pad) { return data_read(pad, 2); }
uint32_t touch_smooth(int pad)    { return data_read(pad, 3); }

void touch_filter(bool on)
{
    if (!on) { REG_CLR(FILTER1, BIT(16)); return; }
    /* IDF defaults: IIR 16 (mode), debounce 1, jitter step 4, smooth IIR 2, hysteresis 2 */
    REG_FIELD(FILTER1, 13, 3, 1); REG_FIELD(FILTER1, 29, 3, 1); REG_FIELD(FILTER1, 9, 4, 4);
    REG_FIELD(FILTER1, 7, 2, 2); REG_FIELD(FILTER1, 1, 2, 2);
    REG_SET(FILTER1, BIT(16));
    REG(CLR) = 0x7FFEu;                                   /* restart the benchmarks from the current value */
}

void touch_threshold(int pad, uint32_t thr)
{
    if (pad < 1 || pad > 14) return;
    REG_FIELD(THN(pad), 16, 16, thr);
}

static touch_fn_t ev_fn; static void *ev_arg; static uint16_t last_active;

RAM_FN static void touch_isr(void *a)
{
    (void)a;
    REG(T_INT_CLR) = T_ACTIVE | T_INACTIVE;
    uint16_t act = REG(T_CHN_ST) & 0x7FFE, ch = act ^ last_active;
    last_active = act;
    for (int n = 1; n <= 14; n++) if ((ch & BIT(n)) && ev_fn) ev_fn(n, (act >> n) & 1, ev_arg);
}

int touch_on_event(touch_fn_t fn, void *arg)
{
    static int line = -1;
    ev_fn = fn; ev_arg = arg;
    if (!fn) { REG_CLR(T_INT_ENA, T_ACTIVE | T_INACTIVE); return 0; }
    last_active = REG(T_CHN_ST) & 0x7FFE;
    if (line < 0 && (line = irq_attach(SRC_LP_TOUCH, 1, touch_isr, 0)) < 0) return -1;
    REG(T_INT_CLR) = T_ACTIVE | T_INACTIVE;
    REG_SET(T_INT_ENA, T_ACTIVE | T_INACTIVE);
    return 0;
}

uint16_t touch_active(void) { return REG(T_CHN_ST) & 0x7FFE; }

void touch_proximity(const int8_t pads[3], uint32_t meas_times)
{
    uint32_t v = 0;
    for (int i = 0; i < 3; i++) v |= (uint32_t)(pads[i] > 0 ? pads[i] : 0) << (4 * i);
    REG(APPROACH) = v;
    REG_FIELD(FILTER1, 21, 8, meas_times);                /* total scans */
    REG_FIELD(APPR_MEAS, 20, 10, 500);                    /* charge times of the proximity channels */
}

uint32_t touch_proximity_count(int i) { return (REG(T_APPR_DATA) >> (16 - 8 * i)) & 0xFF; }

/* The P4 has no reference-capacitor channel: the filter's noise thresholds take that role (IDF touch_denoise level);
 * the level applies at the next touch_init. cap is accepted for API compatibility. */
void touch_denoise(int grade, int cap) { (void)cap; dn_grade = grade; }

uint32_t touch_denoise_data(void) { return 0; }
