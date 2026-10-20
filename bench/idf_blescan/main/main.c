/* A.5 receiver: the vendor BLE stack scanning passively; prints every
 * advertisement whose complete local name starts with "NDF" (sent by
 * esp-ndf rfctl "bleadv", software GFSK through our DAC path), plus a
 * per-second count, channel 37/38/39 rotation by the controller. */
#include <stdio.h>
#include <string.h>
#include "nvs_flash.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_ble_api.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static volatile int hits, others;
static volatile int last_rssi;

static void cb(esp_gap_ble_cb_event_t ev, esp_ble_gap_cb_param_t *p)
{
    if (ev == ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT) esp_ble_gap_start_scanning(0);
    if (ev != ESP_GAP_BLE_SCAN_RESULT_EVT || p->scan_rst.search_evt != ESP_GAP_SEARCH_INQ_RES_EVT) return;
    uint8_t len = 0;
    uint8_t *nm = esp_ble_resolve_adv_data(p->scan_rst.ble_adv, ESP_BLE_AD_TYPE_NAME_CMPL, &len);
    if (nm && len >= 3 && !memcmp(nm, "NDF", 3)) {
        if (hits++ < 5) {
            const uint8_t *a = p->scan_rst.bda;
            printf("adv %02x:%02x:%02x:%02x:%02x:%02x rssi %d name %.*s\n", a[0], a[1], a[2], a[3], a[4], a[5], p->scan_rst.rssi, len, nm);
        }
        last_rssi = p->scan_rst.rssi;
    } else others++;
}

void app_main(void)
{
    nvs_flash_init();
    esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);
    esp_bt_controller_config_t cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    esp_bt_controller_init(&cfg);
    esp_bt_controller_enable(ESP_BT_MODE_BLE);
    esp_bluedroid_init();
    esp_bluedroid_enable();
#ifdef ADV
    /* advertiser for the software receiver (rfctl blerx): ADV_NONCONN_IND, name "IDF-ADV", channel 37 only, 20 ms */
    static uint8_t raw[] = { 2, 1, 6, 8, 9, 'I', 'D', 'F', '-', 'A', 'D', 'V' };
    esp_ble_gap_config_adv_data_raw(raw, sizeof raw);
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_ble_adv_params_t ap = { .adv_int_min = 0x20, .adv_int_max = 0x20, .adv_type = ADV_TYPE_NONCONN_IND,
                                .own_addr_type = BLE_ADDR_TYPE_PUBLIC, .channel_map = ADV_CHNL_37, .adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY };
    esp_ble_gap_start_advertising(&ap);
    printf("advertiser ready\n");
    for (;;) vTaskDelay(1000);
#endif
    esp_ble_gap_register_callback(cb);
    esp_ble_scan_params_t sp = { .scan_type = BLE_SCAN_TYPE_PASSIVE, .own_addr_type = BLE_ADDR_TYPE_PUBLIC,
                                 .scan_filter_policy = BLE_SCAN_FILTER_ALLOW_ALL, .scan_interval = 0x50, .scan_window = 0x50,
                                 .scan_duplicate = BLE_SCAN_DUPLICATE_DISABLE };
    esp_ble_gap_set_scan_params(&sp);
    printf("scanner ready\n");
    for (int s = 0;; s++) {
        int h = hits, o = others;
        vTaskDelay(pdMS_TO_TICKS(1000));
        printf("t %d s: NDF %d/s (rssi %d), other adverts %d/s\n", s, hits - h, last_rssi, others - o);
    }
}
