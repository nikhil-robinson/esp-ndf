/* RF trace: every call through the ROM PHY function table (rom_phyFuns),
 * with arguments, return value and nesting depth, during PHY init and a set
 * of marked phases (tuning, RX gain, TX). Logged to PSRAM, then printed. */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_phy_init.h"
void esp_wifi_bt_power_domain_on(void);
extern void set_chanfreq(uint32_t mhz, uint32_t mode);
extern void set_rf_freq_offset(uint32_t xtal_sel, uint32_t mhz, int32_t khz);
extern void force_rx_gain(uint32_t manual, uint32_t index, uint32_t unused);
extern void set_rx_gain_table(uint32_t mhz, uint32_t flags);
extern void stop_tx_tone(uint32_t wait);
extern void txcal_debuge_mode(void);
extern void txcal_work_mode(void);
extern void start_tx_tone_step(uint32_t on, int32_t a, int32_t atten, uint32_t b, int32_t c, int32_t d);
extern void force_txon_mode(uint32_t en, uint32_t mode, uint32_t ofs);
extern void force_tx_gain(uint32_t gain, uint32_t bb, int32_t dig);

typedef struct { uint16_t idx; uint8_t depth, done; uint32_t a[6]; uint32_t ret; } rec_t;   /* 32 bytes */
static uint32_t NREC;
static rec_t *recs;
static volatile uint32_t nrec, lost;
static uint8_t depth, in_i2c;


static IRAM_ATTR int trace_enter(int idx, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5,
                                 uint32_t a6, uint32_t a7, uint32_t a8, uint32_t a9, uint32_t a10, uint32_t a11, uint32_t a12, uint32_t a13)
{
    (void)a12; (void)a13;
    uint32_t n = nrec;
    if (in_i2c) { depth++; return -1; }                 /* inside an analog-bus primitive: the outer call is the record */
    if (!recs || n >= NREC) { lost++; depth++; return -1; }
    if (idx >= 0x154 / 4 && idx <= 0x198 / 4) in_i2c = 1;
    nrec = n + 1;
    rec_t *r = &recs[n];
    (void)a6; (void)a7; (void)a8; (void)a9; (void)a10; (void)a11;
    r->idx = idx; r->depth = depth++; r->done = 0;
    r->a[0] = a0; r->a[1] = a1; r->a[2] = a2; r->a[3] = a3; r->a[4] = a4; r->a[5] = a5;
    return (int)n;
}
static IRAM_ATTR void trace_exit(int n, uint32_t v)
{
    depth--;
    if (n >= 0) { recs[n].ret = v; recs[n].done = 1; if (recs[n].idx >= 0x154 / 4 && recs[n].idx <= 0x198 / 4) in_i2c = 0; }
}
static void mark(uint32_t id, uint32_t a, uint32_t b)            /* phase marker: idx 0xFFFF */
{
    uint32_t n = nrec;
    if (n >= NREC) return;
    nrec = n + 1;
    memset(&recs[n], 0, sizeof recs[n]);
    recs[n].idx = 0xFFFF; recs[n].a[0] = id; recs[n].a[1] = a; recs[n].a[2] = b;
}

#include "hooks.inc"

static uint32_t *table(void) { return *(uint32_t **)0x3fcef3d4; }

static int install(void)                  /* hook every code entry; returns entries hooked */
{
    uint32_t *t = table();
    int k = 0;
    for (int i = 0; i < 256; i++) {
        if (!hook_fn[i]) continue;
        if (t[i] == (uint32_t)hook_fn[i]) continue;
        orig[i] = (fn14_t)t[i];
        t[i] = (uint32_t)hook_fn[i];
        k++;
    }
    return k;
}

void app_main(void)
{
    vTaskDelay(pdMS_TO_TICKS(1500));
    size_t big = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) - 16384;
    NREC = big / sizeof(rec_t);
    recs = heap_caps_malloc(NREC * sizeof(rec_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    printf("@info nrec %lu\n", (unsigned long)NREC);
    printf("@info table_before_init %p recs %p\n", table(), recs);
    int pre = table() ? install() : 0;
    printf("@info hooked_before_init %d\n", pre);

    mark(1, 0, 0);                                         /* phase 1: power on + PHY init + full calibration */
    esp_wifi_bt_power_domain_on();
    esp_phy_enable(PHY_MODEM_WIFI);
    int post = install();                                  /* entries libphy rewrote during init */
    printf("@info rehooked_after_init %d\n", post);
    for (int i = 0; i < 256; i++) if (hook_fn[i] && orig[i]) printf("@orig %03x %08lx\n", i * 4, (unsigned long)orig[i]);

    /* keep the PHY clocks on for the direct calls below (esp_phy_enable dropped the calibration clock) */
    *(volatile uint32_t *)0x60026014 |= 0x00400000;

    printf("@info init_records %lu lost %lu\n", (unsigned long)nrec, (unsigned long)lost);

    static const uint16_t ch[] = { 2412, 2437, 2462, 2484 };
    for (unsigned i = 0; i < 4; i++) { mark(2, ch[i], 0); set_chanfreq(ch[i], 0); }
    static const uint16_t fr[] = { 1900, 2437, 2700 };
    for (unsigned i = 0; i < 3; i++) { mark(3, fr[i], 0); set_chanfreq(2437, 0); set_rf_freq_offset(0, fr[i], 0); }
    mark(4, 2437, 1); set_chanfreq(2437, 0); set_rf_freq_offset(0, 2437, 125);
    for (int k = 0; k < 4; k++) { mark(12, 2360, 320); set_chanfreq(2437, 0); set_rf_freq_offset(0, 2360, 320); }
    for (int g = 0; g <= 80; g += 20) { mark(5, g, 0); force_rx_gain(1, g, 0); }
    mark(6, 0, 0); force_rx_gain(0, 0, 0);
    mark(7, 2437, 0); set_rx_gain_table(2437, 0);
    mark(8, 0, 0); txcal_debuge_mode(); start_tx_tone_step(1, 0, 10, 0, 0, 0);
    vTaskDelay(1);
    mark(9, 0, 0); start_tx_tone_step(0, 0, 0, 0, 0, 0); txcal_work_mode();
    mark(10, 0, 0); stop_tx_tone(1); force_tx_gain(10, 2, 0); force_txon_mode(1, 0, 0);
    mark(11, 0, 0); force_txon_mode(0, 0, 0);
    mark(99, 0, 0);

    printf("@info records %lu lost %lu\n", (unsigned long)nrec, (unsigned long)lost);
    for (uint32_t n = 0; n < nrec; n++) {
        rec_t *r = &recs[n];
        printf("%04x %02x %d %08lx", r->idx, r->depth, r->done, (unsigned long)r->ret);
        for (int k = 0; k < 6; k++) printf(" %lx", (unsigned long)r->a[k]);
        printf("\n");
        if ((n & 255) == 255) vTaskDelay(1);
    }
    printf("=== done\n");
}
