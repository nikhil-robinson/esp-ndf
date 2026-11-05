/* P4 SAR ADC, same API as drivers/adc/adc.c + adc_cont.c (adc.h). ESP-IDF v6.1: esp_hal_ana_conv/esp32p4 adc_ll.h,
 * hw_ver3 adc_reg.h / lp_adc_reg.h. Units: ADC1 channels 0-7 = GPIO16-23, ADC2 channels 0-5 = GPIO49-54.
 *   one-shot  : LP_ADC (RTC controller), software started, like the S3's SENS path;
 *   continuous: the digital controller (ADC block) walks a pattern table on a timer trigger and streams 4-byte results
 *               through GDMA (AHB, id 8) into a descriptor ring, one descriptor per frame; hardware IIR filters and
 *               threshold monitors as on the S3.
 * mV conversion: the P4 uses a different per-chip calibration than the S3's curve fit and this driver does not carry it, so
 * adc_calibrated() is false and adc_raw_to_mv / adc_read_mv return -1 (the API's "chip has no calibration"). */
#include "adc.h"
#include "spin.h"
#include "regi2c.h"
#include "clk.h"
#include "gpio.h"
#include "gdma.h"
#include "irq.h"
#include "cpu.h"
#include "sys.h"
#include "soc.h"

#define LPADC            (LPPERIPH + 0x7000)
#define LP_READER(u)     (LPADC + ((u) == 1 ? 0x00 : 0x24))   /* b7:0 clk div; b28 (ADC1) / b29 (ADC2) data_inv */
#define LP_MEAS_CTRL2(u) (LPADC + ((u) == 1 ? 0x0C : 0x30))   /* b15:0 data b16 done b17 start b18 start_force b30:19 en_pad b31 en_pad_force */
#define LP_MEAS_MUX(u)   (LPADC + ((u) == 1 ? 0x10 : 0x34))   /* ADC1 b31 dig_force; ADC2 b31 rtc_force */
#define LP_ATTEN(u)      (LPADC + ((u) == 1 ? 0x14 : 0x38))   /* 2 bits per pad (ADC2: pad = ch + 2) */
#define LP_FORCE_XPD     (LPADC + 0x3C)                       /* b1:0 ADC1, b3:2 ADC2: 3 power up, 2 power down, 0 FSM */
#define LPPERI_CLK_EN    (LPPERIPH + 0x0)                      /* b23 LP_ADC clock */
#define ADC              (HPPERIPH1 + 0x1E000)
#define D_CTRL           (ADC + 0x00)   /* b3:2 work_mode b4 sar_sel b5 clk_gated b13:6 clk_div b17:14 sar1_patt_len b22 sar1_patt_p_clear b24 data_sar_sel b27:26 xpd1 force */
#define D_CTRL2          (ADC + 0x04)   /* b0 meas_num_limit b11 timer_sel b23:12 timer_target b24 timer_en */
#define D_FILT1          (ADC + 0x08)   /* b28:26 factor1 b31:29 factor0 */
#define D_FSM_WAIT       (ADC + 0x0C)   /* b7:0 xpd b15:8 rstb b23:16 standby */
#define D_PATT1(i)       (ADC + 0x18 + 4 * (i))
#define D_FILT0          (ADC + 0x3C)   /* b18:14 channel1 b23:19 channel0 b31 reset */
#define D_THRES(i)       (ADC + 0x44 + 4 * (i))   /* b4:0 channel b17:5 high b30:18 low */
#define D_THR_CTRL       (ADC + 0x4C)   /* b30 thres1_en b31 thres0_en */
#define D_INT_ENA        (ADC + 0x50)   /* b26 t1 low b27 t0 low b28 t1 high b29 t0 high */
#define D_INT_ST         (ADC + 0x58)
#define D_INT_CLR        (ADC + 0x5C)
#define D_DMA_CONF       (ADC + 0x60)   /* b15:0 eof_num b30 reset_fsm b31 trans */
#define HPC_CTRL22       (HP_CLKRST_BASE + 0x9C)   /* PERI_CLK_CTRL22: b31:30 ADC clock source (2 = PLL_F80M) */
#define HPC_CTRL23       (HP_CLKRST_BASE + 0xA0)   /* b0 adc clk_en, divider fields */
#define REGI2C_SAR       0x69

static uint8_t atten_of[2][8];
static bool up;
static uint32_t cont_div;

bool adc_calibrated(void) { return false; }

void adc_power_up(void)
{
    if (up) return;
    periph_enable(PERIPH_SARADC);
    REG_SET(LPPERI_CLK_EN, BIT(23));
    REG_FIELD(LP_FORCE_XPD, 0, 2, 3); REG_FIELD(LP_FORCE_XPD, 2, 2, 3);       /* both SARs powered */
    REG_SET(D_CTRL, BIT(5));                                                  /* sar clock gate open */
    REG_FIELD(D_CTRL, 26, 2, 3); REG_FIELD(D_CTRL, 28, 2, 3);
    for (int u = 1; u <= 2; u++) {
        REG_FIELD(LP_READER(u), 0, 8, 1);                                     /* sar clk div, as IDF */
        REG_CLR(LP_READER(u), u == 1 ? BIT(28) : BIT(29));                    /* data not inverted */
    }
    regi2c_write_mask(REGI2C_SAR, 2, 6, 4, 4);                                /* DREF = 4 for both units (calibration init) */
    regi2c_write_mask(REGI2C_SAR, 5, 6, 4, 4);
    REG_CLR(LP_MEAS_MUX(1), BIT(31));                                         /* ADC1 on the RTC controller */
    REG_SET(LP_MEAS_MUX(2), BIT(31));                                         /* ADC2: RTC force */
    for (int u = 1; u <= 2; u++) REG_SET(LP_MEAS_CTRL2(u), BIT(18) | BIT(31)); /* software start + pad map */
    up = true;
}

void adc_load_code(int unit, int atten) { (void)unit; (void)atten; }          /* default init code: no per-chip trim carried */

static int pin_of(int unit, int ch) { return unit == 1 ? 16 + ch : 49 + ch; }

int adc_channel(int unit, int ch, adc_atten_t atten)
{
    if (unit < 1 || unit > 2 || ch < 0 || ch > (unit == 1 ? 7 : 5) || atten > 3) return -1;
    adc_power_up();
    int pin = pin_of(unit, ch);
    REG(IO_MUX_BASE + 4 + 4 * pin) = 1u << 12;                                 /* GPIO function, no IE, no pulls */
    REG(pin < 32 ? GPIO_BASE + 0x28 : GPIO_BASE + 0x34) = BIT(pin & 31);       /* output disable */
    int pad = unit == 1 ? ch : ch + 2;
    REG_FIELD(LP_ATTEN(unit), pad * 2, 2, atten);
    atten_of[unit - 1][ch] = (uint8_t)atten;
    return 0;
}

RAM_FN int adc_read_raw(int unit, int ch)
{
    if (!up || unit < 1 || unit > 2 || ch < 0 || ch > (unit == 1 ? 7 : 5)) return -1;
    uint32_t meas = LP_MEAS_CTRL2(unit);
    if (unit == 1) REG_CLR(LP_MEAS_MUX(1), BIT(31));                           /* RTC controller (continuous mode may own ADC1) */
    REG_FIELD(meas, 19, 12, 1u << (unit == 1 ? ch : ch + 2));
    REG_CLR(meas, BIT(17));
    REG_SET(meas, BIT(17));
    if (!SPIN_US(REG(meas) & BIT(16), 10000)) return -1;                      /* conversion done (bounded) */
    return (int)(REG(meas) & 0xFFFF);
}

int adc_raw_to_mv(int unit, adc_atten_t atten, int raw) { (void)unit; (void)atten; (void)raw; return -1; }
int adc_read_mv(int unit, int ch) { (void)unit; (void)ch; return -1; }

/* ---- continuous mode (ADC1) ---------------------------------------------------------------------------------- */
#define MAX_FRAMES 16

static gdma_desc_t ring[MAX_FRAMES] __attribute__((aligned(64)));
static uint32_t *frames_buf;
static int nframes, fsamples, dma = -1;
static volatile uint32_t head;
static const uint32_t *done[MAX_FRAMES];
static uint32_t tail, overruns, sync_err;
static void (*user_fn)(const uint32_t *, int, void *);
static void *user_arg;
static int user_level = 1;

static void frame_isr(void *arg)
{
    (void)arg;
    uint32_t h = head;
    gdma_desc_t *d = &ring[h % nframes];
    if (!d->suc_eof) sync_err++;
    d->suc_eof = 0;
    const uint32_t *f = d->buf;
    done[h % nframes] = f;
    if (user_fn) user_fn(f, fsamples, user_arg);
    head = h + 1;
}

static adc_cont_ch_t pat[12];
static int npat;
static uint32_t interval;

/* digital controller clock: PLL_F80M (src 2) in PERI_CLK_CTRL22 [b?], divider in PERI_CLK_CTRL23 (IDF adc_ll_digi_clk_sel /
 * _controller_clk_div): 80 MHz / 16 = 5 MHz sample clock, as the S3 driver's APB / 16 */
static void controller_clock(void)
{
    REG_FIELD(HPC_CTRL22, 30, 2, 2);        /* reg_adc_clk_src_sel (PERI_CLK_CTRL22 b31:30) */
    REG_FIELD(HPC_CTRL23, 1, 8, 15);        /* reg_adc_clk_div_num = 15 -> /16 */
    REG_SET(HPC_CTRL23, BIT(0));            /* reg_adc_clk_en */
}

static void controller_setup(void)
{
    adc_power_up();
    controller_clock();
    regi2c_write_mask(REGI2C_SAR, 2, 2, 0, 2);                                /* sample cycle 2 */
    REG(D_FSM_WAIT) = 5 | 8u << 8 | 100u << 16;                               /* xpd, rstb, standby waits */
    uint32_t ctrl = REG(D_CTRL);
    ctrl &= ~((3u << 2) | BIT(4) | (0xFFu << 6) | (0xFu << 14));              /* work_mode 0, sar_sel ADC1, clk_div, patt len */
    ctrl |= BIT(5) | 1u << 6 | (uint32_t)(npat - 1) << 14 | BIT(24);
    REG(D_CTRL) = ctrl;
    for (int i = 0; i < 4; i++) REG(D_PATT1(i)) = 0xFFFFFF;
    for (int i = 0; i < npat; i++) {
        uint32_t v = (pat[i].atten & 3) | (pat[i].ch & 0xF) << 2;
        uint32_t idx = (uint32_t)i / 4, off = (uint32_t)(i % 4) * 6;
        uint32_t tab = REG(D_PATT1(idx));
        tab &= ~(0xFC0000u >> off);
        tab |= (v << 18) >> off;
        REG(D_PATT1(idx)) = tab;
    }
    REG_SET(D_CTRL, BIT(22)); REG_CLR(D_CTRL, BIT(22));                       /* pattern pointer to entry 0 */
    REG(D_CTRL2) = (REG(D_CTRL2) & ~((0xFFFu << 12) | BIT(24) | BIT(0))) | BIT(11) | interval << 12;
    REG_FIELD(D_DMA_CONF, 0, 16, (uint32_t)fsamples);
    REG_SET(LP_MEAS_MUX(1), BIT(31));                                         /* ADC1 -> digital controller */
}

int adc_cont_init(const adc_cont_ch_t *chs, int n, uint32_t sample_hz, uint32_t *buf, int frame_samples, int frames)
{
    if (n < 1 || n > 12 || frames < 2 || frames > MAX_FRAMES || frame_samples < 1 || frame_samples > 1023 || !sample_hz) return -1;
    uint32_t iv = 80000000u / 16 / 2 / sample_hz;                             /* clk = 80 MHz / 16, two clocks per tick */
    if (iv < 40 || iv > 4095) return -1;
    adc_cont_stop();
    for (int i = 0; i < n; i++) {
        if (chs[i].ch > 7 || adc_channel(1, chs[i].ch, chs[i].atten)) return -1;
        pat[i] = chs[i];
    }
    npat = n; interval = iv; cont_div = 16;
    frames_buf = buf; nframes = frames; fsamples = frame_samples;
    for (int i = 0; i < frames; i++)
        ring[i] = (gdma_desc_t){ .size = (uint32_t)frame_samples * 4, .owner = 1, .buf = buf + i * frame_samples,
                                 .next = &ring[(i + 1) % frames] };
    if (dma < 0 && (dma = gdma_alloc()) < 0) return -1;
    gdma_connect(dma, GDMA_ADC);
    return gdma_on_in_done(dma, frame_isr, 0, user_level);
}

int adc_cont_on_frame(void (*fn)(const uint32_t *, int, void *), void *arg, int level)
{
    user_fn = fn; user_arg = arg; user_level = level;
    return dma < 0 ? 0 : gdma_on_in_done(dma, frame_isr, 0, level);
}

int adc_cont_start(void)
{
    if (dma < 0) return -1;
    head = tail = overruns = sync_err = 0;
    controller_setup();
    REG_CLR(D_DMA_CONF, BIT(31));
    REG_SET(D_DMA_CONF, BIT(30)); REG_CLR(D_DMA_CONF, BIT(30));               /* controller FSM reset */
    for (int i = 0; i < nframes; i++) { ring[i].length = 0; ring[i].suc_eof = 0; ring[i].owner = 1; }
    gdma_connect(dma, GDMA_ADC);
    gdma_in_start(dma, &ring[0]);
    REG_SET(D_DMA_CONF, BIT(31));                                             /* results -> DMA */
    REG_SET(D_CTRL2, BIT(24));                                                /* timer trigger on */
    return 0;
}

void adc_cont_stop(void)
{
    if (!(REG(D_CTRL2) & BIT(24))) return;
    REG_CLR(D_CTRL2, BIT(24));
    delay_us(2 * 1000000 / (80000000u / 32 / interval) + 2);                  /* let the conversion in flight finish */
    REG_CLR(D_DMA_CONF, BIT(31));
    if (dma >= 0) gdma_stop(dma);
}

const uint32_t *adc_cont_get(int *samples)
{
    uint32_t h = READ_ONCE(head);
    if (tail == h) return NULL;
    if (h - tail >= (uint32_t)nframes) { overruns += h - tail - 1; tail = h - 1; }
    const uint32_t *f = done[tail % nframes];
    tail++;
    if (samples) *samples = fsamples;
    return f;
}

uint32_t adc_cont_overruns(void) { return overruns; }
uint32_t adc_cont_sync_errors(void) { return sync_err; }

int adc_cont_filter(int idx, int ch, int k)
{
    if (idx < 0 || idx > 1) return -1;
    uint32_t code = k == 2 ? 1 : k == 4 ? 2 : k == 8 ? 3 : k == 16 ? 4 : k == 64 ? 6 : 0;
    if (k && !code) return -1;
    uint32_t sh = idx ? 14 : 19, fs = idx ? 26 : 29;
    REG_FIELD(D_FILT0, sh, 5, k ? (uint32_t)(ch & 0xF) : 0xF);                 /* 0xF + factor 0 = off */
    REG_FIELD(D_FILT1, fs, 3, code);
    REG_SET(D_FILT0, BIT(31)); REG_CLR(D_FILT0, BIT(31));
    return 0;
}

static adc_mon_fn_t mon_fn; static void *mon_arg; static int mon_int = -1;

RAM_FN static void mon_isr(void *a)
{
    (void)a;
    uint32_t st = REG(D_INT_ST) & (0xFu << 26);
    REG(D_INT_CLR) = st;
    if (!mon_fn) return;
    if (st & BIT(29)) mon_fn(0, true, mon_arg);
    if (st & BIT(27)) mon_fn(0, false, mon_arg);
    if (st & BIT(28)) mon_fn(1, true, mon_arg);
    if (st & BIT(26)) mon_fn(1, false, mon_arg);
}

int adc_cont_monitor(int idx, int ch, int low, int high, adc_mon_fn_t fn, void *arg)
{
    if (idx < 0 || idx > 1) return -1;
    if (mon_int < 0 && (mon_int = irq_attach(SRC_ADC, 1, mon_isr, 0)) < 0) return -1;
    uint32_t hi_b = idx ? BIT(28) : BIT(29), lo_b = idx ? BIT(26) : BIT(27);
    if (!fn) { REG_CLR(D_INT_ENA, hi_b | lo_b); REG_CLR(D_THR_CTRL, idx ? BIT(30) : BIT(31)); return 0; }
    mon_fn = fn; mon_arg = arg;
    REG(D_THRES(idx)) = (uint32_t)(ch & 0xF) | (uint32_t)(high & 0x1FFF) << 5 | (uint32_t)(low & 0x1FFF) << 18;
    REG(D_INT_CLR) = hi_b | lo_b;
    REG_SET(D_INT_ENA, (high >= 0 ? hi_b : 0) | (low >= 0 ? lo_b : 0));
    REG_SET(D_THR_CTRL, idx ? BIT(30) : BIT(31));
    return 0;
}
