/* A.8 coexistence: native Wi-Fi and our blob-free radio taking turns on one
 * chip. Each cycle: Wi-Fi start + scan, esp_wifi_stop, our init + receive of
 * a tone from the other board (rfctl, +2 MHz on 2437 MHz), back to Wi-Fi. */
#include <stdio.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "heap_memory_layout.h"
#include "rf_pll.h"
#include "rf_init.h"
#include "rf_dump.h"

SOC_RESERVE_MEMORY_REGION(0x3FCB0000, 0x3FCE0000, rf_dump_banks);
void rf_delay_us(uint32_t us) { esp_rom_delay_us(us); }

static double bin_db(uint32_t n, double f)
{
    const uint32_t *w = (const uint32_t *)RF_DUMP_BURST_ADDR;
    double c = cos(2 * M_PI * f / 80e6), s = sin(2 * M_PI * f / 80e6), re = 0, im = 0, cr = 1, ci = 0;
    for (uint32_t k = 0; k < n; k++) {
        double i = rf_iq_i(w[k]), q = -rf_iq_q(w[k]);
        re += i * cr + q * ci; im += q * cr - i * ci;
        double t = cr * c - ci * s; ci = ci * c + cr * s; cr = t;
    }
    return 10 * log10((re * re + im * im) / ((double)n * n) + 1e-6);
}

static void scan(const char *tag)
{
    int64_t t = esp_timer_get_time();
    esp_wifi_start();
    int64_t ts = esp_timer_get_time();
    wifi_scan_config_t sc = { 0 };
    int rc = esp_wifi_scan_start(&sc, true);
    uint16_t n = 0; esp_wifi_scan_get_ap_num(&n);
    wifi_ap_record_t ap[16]; uint16_t m = 16; esp_wifi_scan_get_ap_records(&m, ap);
    int best = -127; for (int k = 0; k < m; k++) if (ap[k].rssi > best) best = ap[k].rssi;
    printf("%s: wifi start %lld us, scan rc %d, %u APs, best %d dBm\n", tag, ts - t, rc, n, best);
}

static void ours(void)
{
    int64_t t = esp_timer_get_time();
    esp_wifi_stop();
    int64_t t1 = esp_timer_get_time();
    rf_init_replay();
    rf_pll_result_t r; rf_pll_set_khz(2436000, &r);
    rf_rx_on(); rf_rx_gain(20);
    int64_t t2 = esp_timer_get_time();
    esp_rom_delay_us(500);
    rf_dump_burst(RF_DUMP_80M, 8192);
    printf("ours: wifi stop %lld us, our init+tune %lld us, locked %d, tone +3MHz %.1f dB, floor +7MHz %.1f dB\n",
           t1 - t, t2 - t1, r.locked, bin_db(8192, 3e6), bin_db(8192, 7e6));
}

void app_main(void)
{
    vTaskDelay(pdMS_TO_TICKS(1500));
    nvs_flash_init();
    esp_event_loop_create_default();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    esp_wifi_set_mode(WIFI_MODE_STA);
    printf("--- ours before any wifi\n"); ours();
    for (int k = 0; k < 3; k++) {
        printf("--- cycle %d\n", k);
        scan("wifi");
        ours();
    }
    scan("wifi final");
    printf("=== done\n");
    for (;;) vTaskDelay(1000);
}
