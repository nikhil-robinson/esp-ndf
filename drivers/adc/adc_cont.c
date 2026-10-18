/* SAR ADC1 continuous mode: digital controller (APB_SARADC) walks a pattern
 * table on a timer trigger and streams 4-byte results through GDMA into a
 * descriptor ring, one descriptor per frame (EOF every frame_samples).
 * The DMA interrupt counts frames (and calls the user), the reader follows. */
#include "adc.h"
#include "gdma.h"
#include "regi2c.h"
#include "clk.h"
#include "irq.h"
#include "sys.h"

#define SAR_BASE      0x60040000
#define SAR_CTRL      (SAR_BASE + 0x00)   /* b6 clk_gated b7..14 clk_div b15..18 sar1_patt_len b23 sar1 patt clear b25 data_sar_sel */
#define SAR_CTRL2     (SAR_BASE + 0x04)   /* b0 meas_num_limit b11 timer_sel b12..23 timer_target b24 timer_en */
#define SAR_FSM_WAIT  (SAR_BASE + 0x0C)
#define SAR_PATT1(i)  (SAR_BASE + 0x18 + 4 * (i))
#define SAR_DMA_CONF  (SAR_BASE + 0x6C)   /* b0..15 eof_num b30 reset_fsm b31 trans */
#define SAR_CLKM      (SAR_BASE + 0x70)   /* b0..7 div_num b8..13 div_b b14..19 div_a b20 clk_en b21..22 clk_sel */
#define SENS_MEAS1_MUX 0x60008810         /* b31 sar1_dig_force */
#define REGI2C_SAR    0x69

#define MAX_FRAMES    16

static gdma_desc_t ring[MAX_FRAMES];
static uint32_t *frames_buf;
static int nframes, fsamples, dma = -1;
static volatile uint32_t head;              /* frames finished by DMA */
static const uint32_t *done[MAX_FRAMES];    /* finished frames, in DMA order (from the EOF descriptor) */
static uint32_t tail, overruns;             /* frames taken by the reader */
static uint32_t sync_err;                   /* interrupt for a descriptor the DMA had not closed */
static void (*user_fn)(const uint32_t *, int, void *);
static void *user_arg;
static int user_level = 1;

static void frame_isr(void *arg)
{
    (void)arg;
    uint32_t h = head;
    gdma_desc_t *d = &ring[h % nframes];                     /* channel reset at start: DMA fills the ring in order */
    if (!d->suc_eof) sync_err++;                             /* never seen on hardware; checked in tests */
    d->suc_eof = 0;
    const uint32_t *f = d->buf;
    done[h % nframes] = f;
    if (user_fn) user_fn(f, fsamples, user_arg);
    head = h + 1;
}

static adc_cont_ch_t pat[12];
static int npat;
static uint32_t interval;

/* the full controller setup, applied on every start (as IDF does): the
 * digital controller does not keep a usable state across RTC hand-overs */
static void controller_setup(void)
{
    adc_power_up();
    adc_load_code(1, pat[npat - 1].atten);                   /* one init code per unit, as IDF */
    regi2c_write_mask(REGI2C_SAR, 2, 2, 0, 2);              /* sample cycle 2 */
    REG(SAR_FSM_WAIT) = 5 | 8 << 8 | 100 << 16;             /* xpd, rstb, standby waits */
    REG(SAR_CLKM) = 15 | 1 << 8 | 0 << 14 | BIT(20) | 2 << 21;   /* APB / 16 */
    uint32_t ctrl = REG(SAR_CTRL);
    ctrl &= ~((0xFFu << 7) | (0xFu << 15) | (3u << 3) | BIT(5));   /* clk_div, patt len, work_mode 0, sar_sel ADC1 */
    ctrl |= BIT(6) | 1u << 7 | (uint32_t)(npat - 1) << 15 | BIT(25);
    REG(SAR_CTRL) = ctrl;
    for (int i = 0; i < 4; i++) REG(SAR_PATT1(i)) = 0;
    for (int i = 0; i < npat; i++) {
        uint32_t v = (pat[i].atten & 3) | (pat[i].ch & 0xF) << 2;
        REG(SAR_PATT1(i / 4)) |= (v << 18) >> ((i % 4) * 6);
    }
    REG_SET(SAR_CTRL, BIT(23)); REG_CLR(SAR_CTRL, BIT(23));   /* pattern pointer to entry 0 */
    REG(SAR_CTRL2) = (REG(SAR_CTRL2) & ~((0xFFFu << 12) | BIT(24) | BIT(0))) | BIT(11) | interval << 12;
    REG_FIELD(SAR_DMA_CONF, 0, 16, fsamples);
    REG_SET(SENS_MEAS1_MUX, BIT(31));                        /* ADC1 -> digital controller */
}

int adc_cont_init(const adc_cont_ch_t *chs, int n, uint32_t sample_hz, uint32_t *buf, int frame_samples, int frames)
{
    if (n < 1 || n > 12 || frames < 2 || frames > MAX_FRAMES || frame_samples < 1 || frame_samples > 1023) return -1;
    uint32_t iv = clk_apb_hz() / 16 / 2 / sample_hz;         /* clkm = apb / (15 + 1), two clocks per tick */
    if (iv < 40 || iv > 4095) return -1;                     /* 611 .. 83333 Hz at 80 MHz APB */
    adc_cont_stop();
    for (int i = 0; i < n; i++) {
        if (chs[i].ch > 9 || adc_channel(1, chs[i].ch, chs[i].atten)) return -1;   /* pad -> analog */
        pat[i] = chs[i];
    }
    npat = n; interval = iv;
    frames_buf = buf; nframes = frames; fsamples = frame_samples;
    for (int i = 0; i < frames; i++) {
        ring[i] = (gdma_desc_t){ .size = frame_samples * 4, .owner = 1, .buf = buf + i * frame_samples,
                                 .next = &ring[(i + 1) % frames] };
    }
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
    REG_CLR(SAR_DMA_CONF, BIT(31));
    REG_SET(SAR_DMA_CONF, BIT(30)); REG_CLR(SAR_DMA_CONF, BIT(30));   /* controller FSM reset */
    for (int i = 0; i < nframes; i++) { ring[i].length = 0; ring[i].suc_eof = 0; ring[i].owner = 1; }
    gdma_connect(dma, GDMA_ADC);                             /* channel reset: restart at ring[0], empty FIFO */
    gdma_in_start(dma, &ring[0]);
    REG_SET(SAR_DMA_CONF, BIT(31));                          /* results -> DMA */
    REG_SET(SAR_CTRL2, BIT(24));                             /* timer trigger on */
    return 0;
}

void adc_cont_stop(void)
{
    if (!(REG(SAR_CTRL2) & BIT(24))) return;
    REG_CLR(SAR_CTRL2, BIT(24));                             /* no new triggers */
    delay_us(2 * 1000000 / (clk_apb_hz() / 32 / interval) + 2);   /* let the conversion in flight finish */
    REG_CLR(SAR_DMA_CONF, BIT(31));
    if (dma >= 0) gdma_stop(dma);
    /* ADC1 stays on the digital controller; one-shot reads take it back */
}

const uint32_t *adc_cont_get(int *samples)
{
    uint32_t h = READ_ONCE(head);
    if (tail == h) return NULL;
    if (h - tail >= (uint32_t)nframes) {                     /* the DMA lapped the reader */
        overruns += h - tail - 1;
        tail = h - 1;
    }
    const uint32_t *f = done[tail % nframes];
    tail++;
    if (samples) *samples = fsamples;
    return f;
}

uint32_t adc_cont_overruns(void) { return overruns; }
uint32_t adc_cont_sync_errors(void) { return sync_err; }
