/* Receive path: vendor PHY (OURS=0) vs our blob-free code (OURS=1), same
 * board. Both: bring-up, tune 2437 MHz, receive on, then for a range of
 * forced gains a 16383-word capture at 80 MS/s (DC, RMS, peak) and a dump of
 * the modem registers and every analog register the init touches. */
#include <stdio.h>
#include <stdint.h>
#include <math.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "heap_memory_layout.h"
#include "rf_regi2c.h"
#include "rf_pll.h"
#include "rf_init.h"
#include "rf_dump.h"
#include "rf_init_replay_c948.h"
#if BISECT > 9
#include "bisect.h"
#endif

SOC_RESERVE_MEMORY_REGION(0x3FCB0000, 0x3FCE0000, rf_dump_banks);
void rf_delay_us(uint32_t us) { esp_rom_delay_us(us); }

#if !OURS
#include "esp_phy_init.h"
#include "nvs_flash.h"
#include "esp_wifi.h"
#include "esp_event.h"
void esp_wifi_bt_power_domain_on(void);
extern void stop_tx_tone(uint32_t), rom_pbus_workmode(void), rom_pbus_xpd_tx_off(void);
extern void rom_pbus_xpd_rx_on(uint32_t), rom_set_rxclk_en(uint32_t);
extern void set_chanfreq(uint32_t, uint32_t), force_rx_gain(uint32_t, uint32_t, uint32_t);
extern void rom_pbus_xpd_tx_on(uint32_t, uint32_t), force_tx_gain(uint32_t, uint32_t, uint32_t), force_txon_mode(uint32_t, uint32_t, uint32_t);
extern uint8_t phy_param[];
int cmd_parse(char *c, char *n, int *a, char **v) { (void)c; (void)n; (void)a; (void)v; return -1; }
#endif

static const struct { uint32_t base, len; } blks[] = {
    { 0x60005000, 0x1000 }, { 0x60006000, 0x200 }, { 0x6001C000, 0x2000 }, { 0x60033000, 0x3000 }, { 0x6000E000, 0x200 },
};

static void dump_regs(void)
{
    printf("@regs\n");
    for (unsigned b = 0; b < sizeof blks / sizeof blks[0]; b++) {
        for (uint32_t a = blks[b].base; a < blks[b].base + blks[b].len; a += 4)
            printf("%08lx %08lx\n", (unsigned long)a, (unsigned long)*(volatile uint32_t *)a);
        vTaskDelay(1);
    }
    static uint8_t seen[16][256];
    for (unsigned i = 0; i < sizeof rf_init_ops / sizeof rf_init_ops[0]; i++)
        if (rf_init_ops[i].op == 1) seen[rf_init_ops[i].a & 15][rf_init_ops[i].b] = 1;
    for (unsigned b = 0; b < 16; b++)
        for (unsigned r = 0; r < 256; r++)
            if (seen[b][r]) printf("a%02x.%02x %02x\n", 0x60 + b, r, rf_regi2c_read((uint8_t)(0x60 + b), (uint8_t)r));
}

static void wide(const char *tag)
{
    static const uint32_t r[][2] = { { 0x60026000, 0x100 }, { 0x600C0000, 0x200 }, { 0x60008000, 0x140 }, { 0x600C1000, 0x40 },
        { 0x60005000, 0x1000 }, { 0x60006000, 0x200 }, { 0x6001C000, 0x2000 }, { 0x60033000, 0x3000 }, { 0x6000E000, 0x200 } };
    printf("@%s\n", tag);
    for (unsigned b = 0; b < sizeof r / sizeof r[0]; b++)
        for (uint32_t a = r[b][0]; a < r[b][0] + r[b][1]; a += 4) printf("%08lx %08lx\n", (unsigned long)a, (unsigned long)*(volatile uint32_t *)a);
}

/* power at f (Hz) in the last capture, Goertzel over n words at 80 MS/s */
static double bin_power(uint32_t n, double f)
{
    const uint32_t *w = (const uint32_t *)RF_DUMP_BURST_ADDR;
    double c = cos(2 * M_PI * f / 80e6), s = sin(2 * M_PI * f / 80e6), re = 0, im = 0, cr = 1, ci = 0;
    for (uint32_t k = 0; k < n; k++) {
        double i = rf_iq_i(w[k]), q = rf_iq_q(w[k]);
        re += i * cr + q * ci; im += q * cr - i * ci;    /* (i + jq) e^{-j w k} */
        double t = cr * c - ci * s; ci = ci * c + cr * s; cr = t;
    }
    return (re * re + im * im) / ((double)n * n);
}

static void tx_test(void)
{
    const uint32_t n = 2000;                           /* 2 MHz at 40 MS/s: 20 words a period */
    volatile uint32_t *b = (volatile uint32_t *)RF_DUMP_BANK(1);
    for (uint32_t k = 0; k < n; k++)
        b[k] = rf_iq_word((int)lrint(300 * cos(2 * M_PI * k / 20)), (int)lrint(300 * sin(2 * M_PI * k / 20)));
    for (int on = 0; on < 2; on++) {
        if (on) {
#if OURS
            rf_tx_on(127, 0);
#else
            stop_tx_tone(1); rom_pbus_workmode(); rom_pbus_xpd_rx_on(0); rom_pbus_xpd_tx_on(1, 0);
            force_tx_gain(127, 0, 0); force_txon_mode(1, 0, 0); esp_rom_delay_us(50);
#endif
            rf_dac_play(1, n, 0, 1);
            printf("@tx\n");
            for (uint32_t a = 0x60006000; a < 0x60006200; a += 4) printf("%08lx %08lx\n", (unsigned long)a, (unsigned long)*(volatile uint32_t *)a);
        }
        for (int g = 0; g <= 60; g += 20) {
            rf_rx_gain(g);
            esp_rom_delay_us(200);
            rf_dump_burst(RF_DUMP_80M, RF_DUMP_MAX);
            printf("tx %s gain %2d: +2MHz %10.1f  -2MHz %10.1f  +5MHz %8.1f\n", on ? "on " : "off", g,
                   bin_power(RF_DUMP_MAX, 2e6), bin_power(RF_DUMP_MAX, -2e6), bin_power(RF_DUMP_MAX, 5e6));
        }
    }
    rf_dac_stop();
#if OURS
    rf_tx_off();
#else
    force_txon_mode(0, 0, 0);
#endif
}

static void capture(int gain)
{
    rf_rx_gain(gain);
    vTaskDelay(2);
    const uint32_t n = RF_DUMP_MAX;
    int rc = rf_dump_burst(RF_DUMP_80M, n);
    const uint32_t *w = (const uint32_t *)RF_DUMP_BURST_ADDR;
    double si = 0, sq = 0, si2 = 0, sq2 = 0; int pk = 0;
    for (uint32_t k = 0; k < n; k++) {
        int i = rf_iq_i(w[k]), q = rf_iq_q(w[k]);
        si += i; sq += q; si2 += (double)i * i; sq2 += (double)q * q;
        if (abs(i) > pk) pk = abs(i);
        if (abs(q) > pk) pk = abs(q);
    }
    double mi = si / n, mq = sq / n;
    printf("gain %3d rc=%d dc=(%6.1f,%6.1f) rms=(%6.1f,%6.1f) peak=%d first=%08lx\n", gain, rc, mi, mq,
           sqrt(si2 / n - mi * mi), sqrt(sq2 / n - mq * mq), pk, (unsigned long)w[0]);
}

void app_main(void)
{
    vTaskDelay(pdMS_TO_TICKS(1500));
#if OURS
    printf("=== ours (no vendor PHY code)\n");
    int64_t t0 = esp_timer_get_time();
    rf_init_replay();
    printf("init %lld us\n", esp_timer_get_time() - t0);
    rf_pll_result_t r;
    printf("pll rc=%d cap=%03x locked=%d\n", rf_pll_set_khz(2437000, &r), r.cap, r.locked);
    rf_rx_on();
#else
    printf("=== vendor\n");
#if WIFI > 9
    {
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        cfg.nvs_enable = 0;
        esp_event_loop_create_default();
        esp_wifi_init(&cfg);
        esp_wifi_set_storage(WIFI_STORAGE_RAM);
        esp_wifi_set_mode(WIFI_MODE_NULL);
        esp_wifi_start();
        esp_wifi_set_ps(WIFI_PS_NONE);
        esp_wifi_set_promiscuous(true);
        esp_wifi_set_channel(6, WIFI_SECOND_CHAN_NONE);
        vTaskDelay(pdMS_TO_TICKS(200));
    }
#else
#if CONFIG_ESP_PHY_CALIBRATION_AND_DATA_STORAGE
    nvs_flash_init();                                  /* stored calibration data (not timed) */
#endif
    int64_t t0 = esp_timer_get_time();
    esp_wifi_bt_power_domain_on();
    esp_phy_enable(PHY_MODEM_WIFI);
    printf("init %lld us\n", esp_timer_get_time() - t0);
#endif
    *(volatile uint32_t *)0x60026014 |= 0x00400000;
    set_chanfreq(2437, 0);
    stop_tx_tone(1);
    *(volatile uint32_t *)0x6002600C |= 2;     /* what esp_wifi_start adds for the dump engine */
    rom_pbus_workmode(); rom_pbus_xpd_tx_off(); rom_pbus_xpd_rx_on(1); rom_set_rxclk_en(1);
    esp_rom_delay_us(100);
#endif
#if WIFI > 9
    capture(40);
    printf("promisc off: "); esp_wifi_set_promiscuous(false); vTaskDelay(10); capture(40);
    printf("ps min: "); esp_wifi_set_ps(WIFI_PS_MIN_MODEM); vTaskDelay(10); capture(40);
    printf("ps none: "); esp_wifi_set_ps(WIFI_PS_NONE); vTaskDelay(10); capture(40);
    esp_phy_enable(PHY_MODEM_BT);
    wide("pre_stop");
    esp_wifi_stop();
    wide("post_stop");
    printf("stop: "); vTaskDelay(10); capture(40);
    printf("rx prep again: "); stop_tx_tone(1); rom_pbus_workmode(); rom_pbus_xpd_tx_off(); rom_pbus_xpd_rx_on(1); rom_set_rxclk_en(1); vTaskDelay(1); capture(40);
#endif
#if BISECT > 9
    capture(40);
    for (unsigned k = 0; k < sizeof wifi_regs / sizeof wifi_regs[0]; k++) {
        printf("set %08lx = %08lx: ", (unsigned long)wifi_regs[k].a, (unsigned long)wifi_regs[k].v);
        fflush(stdout); vTaskDelay(2);
        *(volatile uint32_t *)wifi_regs[k].a = wifi_regs[k].v;
        capture(40);

    }
#endif
#if DUMPWIDE > 9
    {   /* wide register dump after init (vendor or ours), for the cold-boot comparison */
        static const uint32_t r[][2] = { { 0x60005000, 0x60008000 }, { 0x60011000, 0x60012000 }, { 0x6001C000, 0x60020000 },
                                         { 0x60030000, 0x60040000 }, { 0x6000E000, 0x6000E200 }, { 0x60026000, 0x60026100 } };
        extern uint32_t rom_pbus_rd(uint32_t row, uint32_t col);
        for (unsigned row = 0; row < 8; row++) for (unsigned col = 1; col <= 3; col++) printf("pbus %u %u %03lx\n", row, col, (unsigned long)(rom_pbus_rd(row, col) & 0x1FF));
        printf("@wide\n");
        for (unsigned b = 0; b < sizeof r / sizeof r[0]; b++)
            for (uint32_t a = r[b][0]; a < r[b][1]; a += 4) { printf("%08lx %08lx\n", (unsigned long)a, (unsigned long)*(volatile uint32_t *)a); if ((a & 0xFFF) == 0) vTaskDelay(1); }
        for (unsigned b = 0x60; b <= 0x6F; b++) {
            for (unsigned r = 0; r < 256; r++) printf("a%02x%02x %02x\n", b, r, rf_regi2c_read((uint8_t)b, (uint8_t)r));
            vTaskDelay(1);
        }
        printf("=== done\n");
        for (;;) vTaskDelay(100);
    }
#endif
#if IQPROBE > 9
    {   /* vendor TX IQ measurement vs the IQ correction word */
        extern void txcal_debuge_mode(void), txcal_work_mode(void);
        extern void txiq_set_reg(int32_t v, uint32_t which);
        extern void txiq_get_mis_pwr(uint32_t mode, int32_t atten, int32_t freq, int16_t *o1, int16_t *o2);
        printf("607C %08lx\n", (unsigned long)*(volatile uint32_t *)0x6000607C);
        txcal_debuge_mode();
        static const int set[][2] = { { 0, -24 }, { 0, -16 }, { 0, -8 }, { 0, 0 }, { 0, 8 }, { 0, 16 }, { 0, 24 },
                                      { -12, 0 }, { -8, 0 }, { -4, 0 }, { 4, 0 }, { 8, 0 }, { 12, 0 }, { -1, -3 } };
        for (int f = 64; f <= 256; f *= 2)
            for (unsigned k = 0; k < sizeof set / sizeof set[0]; k++) {
                txiq_set_reg(set[k][0], 1); txiq_set_reg(set[k][1], 0);
                int16_t a, b, c, d;
                txiq_get_mis_pwr(1, 0, f, &a, &b);
                txiq_get_mis_pwr(0, 0, f, &c, &d);
                printf("vendor f %3d g %3d p %3d: mode1 %5d %5d  mode0 %5d %5d  607C %03lx tone %08lx\n", f, set[k][0], set[k][1], a, b, c, d,
                       (unsigned long)(*(volatile uint32_t *)0x6000607C & 0x7FF), (unsigned long)*(volatile uint32_t *)0x60006040);
            }
        txcal_work_mode();
    }
#endif
#if LADDER > 9
    {   /* vendor TX power ladder: force_tx_gain(gain, bb) steps, 1.5 s each, DAC tone */
        static const struct { uint8_t g; uint16_t bb; } lad[] = { {0,0},{32,0},{64,0},{96,0},{127,0},{127,0x080},{127,0x100},{127,0x020},{127,0x0a0} };
        const uint32_t n = 2000;
        volatile uint32_t *b = (volatile uint32_t *)RF_DUMP_BANK(1);
        for (uint32_t k = 0; k < n; k++) b[k] = rf_iq_word((int)lrint(300 * cos(2 * M_PI * k / 20)), (int)lrint(300 * sin(2 * M_PI * k / 20)));
        stop_tx_tone(1); rom_pbus_workmode(); rom_pbus_xpd_rx_on(0); rom_pbus_xpd_tx_on(1, 0);
        force_txon_mode(1, 0, 0); rf_dac_play(1, n, 0, 1);
        for (int rep = 0; rep < 3; rep++)
            for (unsigned k = 0; k < sizeof lad / sizeof lad[0]; k++) {
                force_tx_gain(lad[k].g, lad[k].bb, 0);
                printf("step %u gain %u bb %x t %lld\n", k, lad[k].g, lad[k].bb, esp_timer_get_time() / 1000);
                vTaskDelay(pdMS_TO_TICKS(1500));
            }
        for (;;) vTaskDelay(100);
    }
#endif
#if TXLOOP > 9
#ifdef VFUNC
    {   /* pieces of the vendor init after ours */
        extern void phy_get_romfunc_addr(void), register_chipv7_phy_init_param(const void *), rf_init(void), bb_init(void);
        extern const void *esp_phy_get_init_data(void);
        phy_get_romfunc_addr();
        void (**tab)(void) = *(void (***)(void))0x3FCEF3D4;   /* rom_phyFuns, as register_chipv7_phy uses it */
        tab[0x204 / 4](); tab[0x234 / 4]();
        register_chipv7_phy_init_param(esp_phy_get_init_data());
        if (VFUNC & 1) rf_init();
        if (VFUNC & 2) bb_init();
        rf_pll_result_t pr; rf_pll_set_khz(2437000, &pr);
        printf("vendor pieces %d run after our init\n", VFUNC);
    }
#endif
#ifdef ZROWS
    for (int k = ZROWS; k < 256; k++) rf_gain_mem_write((uint8_t)k, 0, 0);   /* rows our replay never writes */
    printf("gain rows %d..255 zeroed\n", ZROWS);
#endif
    {   /* vendor transmit reference: +2 MHz DAC tone, 1 s on / 1 s off, forever */
        const uint32_t n = 2000;
        volatile uint32_t *b = (volatile uint32_t *)RF_DUMP_BANK(1);
        for (uint32_t k = 0; k < n; k++)
            b[k] = rf_iq_word((int)lrint(300 * cos(2 * M_PI * k / 20)), (int)lrint(300 * sin(2 * M_PI * k / 20)));
        for (;;) {
#if OURS || TXOURS > 9
            rf_tx_on(127, 0);
#else
            stop_tx_tone(1); rom_pbus_workmode(); rom_pbus_xpd_rx_on(0); rom_pbus_xpd_tx_on(1, 0);
            force_tx_gain(127, 0, 0); force_txon_mode(1, 0, 0); esp_rom_delay_us(50);
#endif
            rf_dac_play(1, n, 0, 1);
            printf("tx on\n");
            vTaskDelay(pdMS_TO_TICKS(1000));
            rf_dac_stop();
#if OURS || TXOURS > 9
            rf_tx_off();
#else
            force_txon_mode(0, 0, 0);
#endif
            printf("tx off\n");
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
#endif
    static const int gains[] = { 0, 10, 20, 30, 40, 50, 60, 70, 80, 90, 100, 110, 120, -1 };
    for (unsigned g = 0; g < sizeof gains / sizeof gains[0]; g++) capture(gains[g]);
#if !OURS
    printf("/* per-chip TX calibration (phy_param +0x124/+0x14C Wi-Fi, +0x182/+0x180 BT) */\n#pragma once\n#include <stdint.h>\n");
    const uint16_t *pw = (const uint16_t *)(phy_param + 0x124), *pb = (const uint16_t *)(phy_param + 0x182);
    printf("static const uint16_t rf_txcal_wifi[5][4] = {");
    for (int r = 0; r < 5; r++) printf(" { 0x%03X, 0x%03X, 0x%03X, 0x%03X },", pw[4 * r], pw[4 * r + 1], pw[4 * r + 2], pw[4 * r + 3]);
    printf(" }, rf_txcal_wifi2 = 0x%03X;\n", *(const uint16_t *)(phy_param + 0x14C));
    printf("static const uint16_t rf_txcal_bt[3][4] = {");
    for (int r = 0; r < 3; r++) printf(" { 0x%03X, 0x%03X, 0x%03X, 0x%03X },", pb[4 * r], pb[4 * r + 1], pb[4 * r + 2], pb[4 * r + 3]);
    printf(" }, rf_txcal_bt2 = 0x%03X;\n", *(const uint16_t *)(phy_param + 0x180));
#endif
    tx_test();
    rf_rx_gain(40);
    vTaskDelay(2);
    dump_regs();
    printf("=== done\n");
}
