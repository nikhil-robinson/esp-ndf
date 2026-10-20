/* Does libphy's BLE RF test receive packets with the BLE link-layer controller never started?
 * D0 (MAC ..:D0) transmits with esp_phy_ble_tx (continuous, sync word 0x71764129, 37 bytes PRBS9, 1M);
 * C9:48 receives with esp_phy_ble_rx for 3 s while watching the modem (0x60011000) and the link-layer core (0x60031000). */
#include <stdio.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_mac.h"
#include "esp_phy_cert_test.h"

#define R(a) (*(volatile uint32_t *)(a))
static void tx_task(void *a) { esp_phy_test_start_stop(3); esp_phy_ble_tx(8, 0, 37, PHY_BLE_TYPE_prbs9, 0x71764129, PHY_BLE_RATE_1M, 0); vTaskDelete(NULL); }
static volatile int rx_ch;
static void rx_task(void *a) { esp_phy_test_start_stop(3); esp_phy_ble_rx(rx_ch, 0x71764129, PHY_BLE_RATE_1M); vTaskDelete(NULL); }

void app_main(void)
{
    uint8_t mac[6]; esp_read_mac(mac, ESP_MAC_WIFI_STA);
    esp_phy_rftest_config(1);
    esp_phy_rftest_init();
    if (mac[5] == 0xD0) {
        static uint32_t before[256], seq0[256], mod0b[64];
        for (int k = 0; k < 256; k++) { before[k] = R(0x60031000 + 4 * k); seq0[k] = R(0x6000E000 + 4 * k); }
        for (int k = 0; k < 64; k++) mod0b[k] = R(0x60011000 + 4 * k);
        uint32_t b = R(0x60031000); printf("=== tx\n"); xTaskCreatePinnedToCore(tx_task, "tx", 8192, 0, 5, 0, 1);
        vTaskDelay(50);
        for (int k = 0; k < 256; k++) { uint32_t x = R(0x60031000 + 4 * k); if (x != before[k]) printf("core %03x %08lx -> %08lx\n", 4 * k, before[k], x); }
        for (int k = 0; k < 256; k++) { uint32_t x = R(0x6000E000 + 4 * k); if (x != seq0[k]) printf("seq %03x %08lx -> %08lx\n", 4 * k, seq0[k], x); }
        for (int k = 0; k < 64; k++) { uint32_t x = R(0x60011000 + 4 * k); if (x != mod0b[k]) printf("modem %03x %08lx -> %08lx\n", 4 * k, mod0b[k], x); }
        printf("=== txdump\n");
        for (;;) { vTaskDelay(500); printf("tx RWBLECNTL before %08lx now %08lx st %08lx\n", b, R(0x60031000), R(0x60031014)); }
    }
    static const int chs[] = { 0, 1, 37 };
    for (int ci = 0; ci < 3; ci++) {
    rx_ch = chs[ci]; esp_phy_test_start_stop(0); vTaskDelay(20);
    printf("--- rx chan %d\n", rx_ch);
    printf("=== rx (no BT controller)\n");
    static uint32_t core0[48], mod0[64];
    for (int k = 0; k < 48; k++) core0[k] = R(0x60031000 + 4 * k);
    xTaskCreatePinnedToCore(rx_task, "rx", 8192, 0, 5, 0, 1);
    vTaskDelay(10);
    static uint32_t mch[64], cch[48], ml[64], cl[48];
    for (int k = 0; k < 64; k++) ml[k] = mod0[k] = R(0x60011000 + 4 * k);
    for (int k = 0; k < 48; k++) cl[k] = R(0x60031000 + 4 * k);
    TickType_t t0 = xTaskGetTickCount();
    while (xTaskGetTickCount() - t0 < pdMS_TO_TICKS(2000)) {
        for (int k = 0; k < 64; k++) { uint32_t x = R(0x60011000 + 4 * k); if (x != ml[k]) { mch[k]++; ml[k] = x; } }
        for (int k = 0; k < 48; k++) { uint32_t x = R(0x60031000 + 4 * k); if (x != cl[k]) { cch[k]++; cl[k] = x; } }
        if (((xTaskGetTickCount() - t0) & 15) == 0) vTaskDelay(1);
    }
    esp_phy_rx_result_t r; esp_phy_get_rx_result(&r);
    printf("rx result: correct %lu total %lu rssi %d flag %lu\n", r.phy_rx_correct_count, r.phy_rx_total_count, r.phy_rx_rssi, r.phy_rx_result_flag);
    printf("core RWBLECNTL before %08lx during %08lx\n", core0[0], cl[0]);
    for (int k = 0; k < 48; k++) if (cch[k]) printf("core %03x changed %lu last %08lx\n", 4 * k, cch[k], cl[k]);
    for (int k = 0; k < 64; k++) if (mch[k]) printf("modem %03x changed %lu last %08lx\n", 4 * k, mch[k], ml[k]);
    }
    printf("=== done\n");
    for (;;) vTaskDelay(1000);
}
