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
#include "esp_wifi.h"
#include "esp_event.h"
void esp_wifi_bt_power_domain_on(void);
extern void stop_tx_tone(uint32_t), rom_pbus_workmode(void), rom_pbus_xpd_tx_off(void);
extern void rom_pbus_xpd_rx_on(uint32_t), rom_set_rxclk_en(uint32_t);
extern void set_chanfreq(uint32_t, uint32_t), force_rx_gain(uint32_t, uint32_t, uint32_t);
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
    rf_init_replay();
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
    esp_wifi_bt_power_domain_on();
    esp_phy_enable(PHY_MODEM_WIFI);
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
    static const int gains[] = { 0, 10, 20, 30, 40, 50, 60, 70, 80, 90, 100, 110, 120, -1 };
    for (unsigned g = 0; g < sizeof gains / sizeof gains[0]; g++) capture(gains[g]);
    rf_rx_gain(40);
    vTaskDelay(2);
    dump_regs();
    printf("=== done\n");
}
