#include "test.h"
#include "adc.h"
#include "ledc.h"
#include "sys.h"
#include "gpio.h"

/* GPIO10 = ADC1 channel 9, jumpered to GPIO11 (driven by us). */
#define FS      20000
#define FRAME   1000                      /* 50 ms per frame at 20 kHz */
#define NFR     4
extern uint8_t back[32768];                /* shared test buffer (test_fat.c) */
#define buf ((uint32_t *)back)
static volatile uint32_t t_frame[32];
static volatile int nf;

static void on_frame(const uint32_t *f, int n, void *arg)
{
    (void)f; (void)n; (void)arg;
    if (nf < 32) t_frame[nf] = ccount();
    nf++;
}

static uint32_t mean_of(const uint32_t *f, int n, int ch)
{
    uint32_t s = 0, k = 0;
    for (int i = 0; i < n; i++) if ((int)ADC_CONT_CH(f[i]) == ch) { s += ADC_CONT_DATA(f[i]); k++; }
    return k ? s / k : 9999;
}

static const uint32_t *wait_frame(void)
{
    const uint32_t *f; int n;
    uint32_t t = ccount();
    while (!(f = adc_cont_get(&n))) if (ccount() - t > 240000000u) return NULL;
    return f;
}

void test_adc_cont(void)
{
    adc_cont_ch_t one[1] = { { 9, ADC_ATTEN_12DB } };
    CHECK(adc_cont_init(one, 1, FS, buf, FRAME, NFR) == 0);
    /* static levels */
    gpio_config(11, GPIO_OUTPUT);
    gpio_clr(11);
    CHECK(adc_cont_start() == 0);
    wait_frame(); const uint32_t *f = wait_frame();
    uint32_t lo = f ? mean_of(f, FRAME, 9) : 9999;
    gpio_set(11); wait_frame(); f = wait_frame();
    uint32_t hi = f ? mean_of(f, FRAME, 9) : 0;
    adc_cont_stop();
    printf("    GPIO11 low/high: mean raw %lu / %lu\n", lo, hi);
    CHECK(lo < 50 && hi > 4000);

    /* sample clock: 1 kHz square on GPIO11 (LEDC) -> 20 samples per period at 20 kHz */
    ledc_timer(0, 1000, 10);
    ledc_channel(0, 0, 11, 512);
    nf = 0;
    CHECK(adc_cont_on_frame(on_frame, 0, 1) == 0);
    adc_cont_start();
    wait_frame();
    f = wait_frame();
    int rises = 0, first = -1, last = -1;
    for (int i = 1; f && i < FRAME; i++)
        if (ADC_CONT_DATA(f[i - 1]) < 2048 && ADC_CONT_DATA(f[i]) >= 2048) { rises++; if (first < 0) first = i; last = i; }
    while (nf < 12) ;
    adc_cont_stop();
    adc_cont_on_frame(NULL, 0, 1);
    ledc_stop(0, false);
    uint32_t dmin = ~0u, dmax = 0;
    for (int i = 3; i < 12; i++) { uint32_t d = t_frame[i] - t_frame[i - 1]; if (d < dmin) dmin = d; if (d > dmax) dmax = d; }
    int spp1000 = rises > 1 ? (last - first) * 1000 / (rises - 1) : 0;      /* samples per 1 kHz period x1000 */
    printf("    1 kHz square: %d rising edges per frame, %d.%03d samples/period (ideal 20)\n", rises, spp1000 / 1000, spp1000 % 1000);
    printf("    frame interval %lu..%lu cycles = %lu.%04lu ms (ideal 50 ms), jitter %lu cycles\n",
           dmin, dmax, dmin / 240000, dmin % 240000 * 10000 / 240000, dmax - dmin);
    CHECK(rises >= 49 && rises <= 50 && spp1000 > 19900 && spp1000 < 20100);
    CHECK(dmin > 11880000 && dmax < 12120000);

    /* two channels round-robin: tags alternate 9, 0, 9, 0 ... */
    adc_cont_ch_t two[2] = { { 9, ADC_ATTEN_12DB }, { 0, ADC_ATTEN_12DB } };
    CHECK(adc_cont_init(two, 2, FS, buf, FRAME, NFR) == 0);
    gpio_config(11, GPIO_OUTPUT);                            /* take GPIO11 back from LEDC */
    gpio_set(11);
    adc_cont_start(); wait_frame(); f = wait_frame(); adc_cont_stop();
    int order_ok = 0;                                        /* strict alternation, whatever entry a frame starts on */
    for (int i = 0; f && i + 1 < FRAME; i++) order_ok += ADC_CONT_CH(f[i]) + ADC_CONT_CH(f[i + 1]) == 9;
    uint32_t m9 = f ? mean_of(f, FRAME, 9) : 0, m0 = f ? mean_of(f, FRAME, 0) : 0;
    printf("    2 channels: %d/%d neighbours alternate 9/0, ch9 mean %lu (GPIO11 high), ch0 mean %lu (GPIO1 open)\n",
           order_ok, FRAME - 1, m9, m0);
    CHECK(order_ok == FRAME - 1 && m9 > 4000);

    /* mid level (pull-up + pull-down divider on the pad): continuous vs one-shot */
    gpio_config(11, GPIO_INPUT | GPIO_PULLUP | GPIO_PULLDOWN);
    CHECK(adc_cont_init(one, 1, FS, buf, FRAME, NFR) == 0);
    adc_cont_start(); wait_frame(); f = wait_frame(); adc_cont_stop();
    uint32_t cont = f ? mean_of(f, FRAME, 9) : 0;
    adc_channel(1, 9, ADC_ATTEN_12DB);
    uint32_t s = 0; for (int i = 0; i < 256; i++) s += adc_read_raw(1, 9);
    uint32_t one_shot = s / 256;
    printf("    mid level: continuous mean %lu, one-shot mean %lu (%ld mV vs %ld mV)\n", cont, one_shot,
           (long)adc_raw_to_mv(1, ADC_ATTEN_12DB, cont), (long)adc_raw_to_mv(1, ADC_ATTEN_12DB, one_shot));
    CHECK((int)cont - (int)one_shot < 40 && (int)one_shot - (int)cont < 40);
    printf("    overruns %lu, sync errors %lu\n", adc_cont_overruns(), adc_cont_sync_errors());
    CHECK(adc_cont_sync_errors() == 0);
    gpio_config(11, GPIO_INPUT);
    gpio_config(10, GPIO_INPUT);                             /* pad back from analog (TWAI uses 10/11 next) */
    gpio_config(10, GPIO_INPUT);
}

/* IIR filter: peak-to-peak of a 1 kHz square at 20 kHz sampling, raw vs k = 16.
 * Monitors: static low / high on GPIO11 -> only the matching side fires. */
static volatile int mon_hi, mon_lo;
static void on_mon(int idx, bool high, void *a) { (void)idx; (void)a; if (high) mon_hi++; else mon_lo++; }
static uint32_t p2p(const uint32_t *f)
{
    uint32_t lo = 4095, hi = 0;
    for (int i = 0; i < FRAME; i++) { uint32_t v = ADC_CONT_DATA(f[i]); if (v < lo) lo = v; if (v > hi) hi = v; }
    return hi - lo;
}

void test_adc_filter(void)
{
    __asm__ volatile("rsil a2, 0" ::: "a2");
    adc_cont_ch_t one[1] = { { 9, ADC_ATTEN_12DB } };
    ledc_timer(0, 1000, 10);
    ledc_channel(0, 0, 11, 512);
    CHECK(adc_cont_init(one, 1, FS, buf, FRAME, NFR) == 0);
    adc_cont_start(); wait_frame(); const uint32_t *f = wait_frame();
    uint32_t raw = f ? p2p(f) : 0;
    adc_cont_stop();
    CHECK(adc_cont_filter(0, 9, 16) == 0);
    adc_cont_start(); wait_frame(); f = wait_frame();
    uint32_t filt = f ? p2p(f) : 0;
    adc_cont_stop();
    adc_cont_filter(0, 9, 0);
    ledc_stop(0, false);
    /* k = 16 over a 10-sample half period: 1 - (15/16)^10 = 48 % of the step, both ways -> p2p ~ 0.48 / 1.52 */
    printf("    1 kHz square p-p: raw %lu, IIR k=16 %lu (expect ~%lu)\n", raw, filt, raw * 31 / 100);
    CHECK(raw > 3900 && filt > raw / 5 && filt < raw / 2);

    gpio_config(11, GPIO_OUTPUT); gpio_clr(11);
    CHECK(adc_cont_monitor(0, 9, 1000, 3000, on_mon, 0) == 0);
    adc_cont_start(); wait_frame();
    mon_hi = mon_lo = 0; wait_frame();
    int l_hi = mon_hi, l_lo = mon_lo;
    gpio_set(11); wait_frame();
    mon_hi = mon_lo = 0; wait_frame();
    int h_hi = mon_hi, h_lo = mon_lo;
    adc_cont_stop();
    adc_cont_monitor(0, 9, 0, 0, 0, 0);
    printf("    monitor low 1000 / high 3000: input low -> %d low, %d high events; input high -> %d low, %d high (per 50 ms)\n",
           l_lo, l_hi, h_lo, h_hi);
    CHECK(l_lo > 0 && l_hi == 0 && h_hi > 0 && h_lo == 0);
    gpio_config(11, GPIO_INPUT);
    gpio_config(10, GPIO_INPUT);
}
