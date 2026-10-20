/* libphy's BLE RF test mode (no BT controller), dumped: D0 (MAC ..:D0) runs esp_phy_ble_tx with one setting changed
 * at a time and prints the test-mode memory (ET 0x3FCD0100, CS 0x3FCD0200, TX buffer 0x3FCD0300) and core registers.
 * C9:48 runs esp_phy_ble_rx and prints the same plus the RX result. */
#include <stdio.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_mac.h"
#include "esp_phy_cert_test.h"

#define R(a) (*(volatile uint32_t *)(a))
static volatile uint32_t tp[7];
static void tx_task(void *a) { esp_phy_test_start_stop(3); esp_phy_ble_tx(tp[0], tp[1], tp[2], tp[3], tp[4], tp[5], tp[6]); vTaskDelete(NULL); }
static void rx_task(void *a) { esp_phy_test_start_stop(3); esp_phy_ble_rx(tp[1], tp[4], tp[5]); vTaskDelete(NULL); }
static void dump(const char *name)
{
    printf("cfg %s\n", name);
    for (uint32_t base = 0x3FCD0100; base < 0x3FCD0340; base += 0x20) {
        printf("  %08lx:", base);
        for (int k = 0; k < 8; k++) printf(" %08lx", R(base + 4 * k));
        printf("\n");
    }
    static const uint16_t o[] = { 0x00, 0x04, 0x08, 0x24, 0x28, 0x50, 0x54, 0x70, 0x74, 0x78, 0xD0, 0xD4, 0x370, 0x374, 0x204, 0x208, 0x20C, 0x2C4 };
    printf("  core:");
    for (unsigned k = 0; k < sizeof o / 2; k++) printf(" %03x=%08lx", o[k], R(0x60031000 + o[k]));
    printf("\n");
}

void app_main(void)
{
    uint8_t mac[6]; esp_read_mac(mac, ESP_MAC_WIFI_STA);
    esp_phy_rftest_config(1);
    esp_phy_rftest_init();
    if (mac[5] == 0xD0) {                                    /* long-packet test: 200 bytes PRBS9 on 2404 MHz (TX chan 1) */
        tp[0] = 8; tp[1] = 1; tp[2] = 200; tp[3] = PHY_BLE_TYPE_prbs9; tp[4] = 0x71764129; tp[5] = PHY_BLE_RATE_1M; tp[6] = 0;
        printf("=== tx len 200 chan 1\n"); xTaskCreatePinnedToCore(tx_task, "tx", 8192, 0, 5, 0, 1);
        for (;;) vTaskDelay(1000);
    }
    printf("=== rx\n");
    tp[1] = 0; tp[4] = 0x71764129; tp[5] = PHY_BLE_RATE_1M;
    xTaskCreatePinnedToCore(rx_task, "rx", 8192, 0, 5, 0, 1);
    vTaskDelay(30);
    dump("rx 1M sync71764129 ch0");
    printf("rx desc 2c0: %08lx %08lx  2e0: %08lx %08lx  buf b00: %08lx %08lx %08lx  f00: %08lx %08lx %08lx\n", R(0x3FCD02C0), R(0x3FCD02C4), R(0x3FCD02E0), R(0x3FCD02E4), R(0x3FCD0B00), R(0x3FCD0B04), R(0x3FCD0B08), R(0x3FCD0F00), R(0x3FCD0F04), R(0x3FCD0F08));
    printf("modem all:\n"); for (int k = 0; k < 64; k++) { printf(" %03x=%08lx", 4 * k, R(0x60011000 + 4 * k)); if ((k & 7) == 7) printf("\n"); }
    printf("core all:\n"); for (int k = 0; k < 256; k++) { printf(" %03x=%08lx", 4 * k, R(0x60031000 + 4 * k)); if ((k & 7) == 7) printf("\n"); }
    vTaskDelay(200);
    esp_phy_rx_result_t r; esp_phy_get_rx_result(&r);
    printf("rx result: correct %lu total %lu rssi %d flag %lu\n", r.phy_rx_correct_count, r.phy_rx_total_count, r.phy_rx_rssi, r.phy_rx_result_flag);
    printf("=== done\n");
    for (;;) vTaskDelay(1000);
}
