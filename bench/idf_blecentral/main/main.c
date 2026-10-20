/* BLE central for the connection tests (runs on D0, UART console): scans for an advertiser whose
 * name starts with TARGET, connects (GATT client), reports connection state, MTU and discovered
 * services, then stays connected and prints the link state every second. */
#include <stdio.h>
#include <string.h>
#include "nvs_flash.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_ble_api.h"
#include "esp_gattc_api.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define TARGET "NDF"
static esp_gatt_if_t gif;
static volatile int connected, opening, conn_id_v, nsvc, nnot;
static esp_bd_addr_t peer;

static void gap_cb(esp_gap_ble_cb_event_t ev, esp_ble_gap_cb_param_t *p)
{
    if (ev == ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT) esp_ble_gap_start_scanning(0);
    if (ev == ESP_GAP_BLE_SCAN_RESULT_EVT && p->scan_rst.search_evt == ESP_GAP_SEARCH_INQ_RES_EVT && !opening) {
        uint8_t len = 0; uint8_t *nm = esp_ble_resolve_adv_data(p->scan_rst.ble_adv, ESP_BLE_AD_TYPE_NAME_CMPL, &len);
        if (nm && len >= strlen(TARGET) && !memcmp(nm, TARGET, strlen(TARGET)) && p->scan_rst.ble_evt_type == ESP_BLE_EVT_CONN_ADV) {
            opening = 1; memcpy(peer, p->scan_rst.bda, 6);
            printf("found %.*s %02x:%02x:%02x:%02x:%02x:%02x type %d rssi %d: connecting\n", len, nm, peer[0], peer[1], peer[2], peer[3], peer[4], peer[5],
                   p->scan_rst.ble_addr_type, p->scan_rst.rssi);
            esp_ble_gap_stop_scanning();
            esp_ble_gattc_open(gif, peer, p->scan_rst.ble_addr_type, true);
        }
    }
    if (ev == ESP_GAP_BLE_UPDATE_CONN_PARAMS_EVT)
        printf("conn params: interval %d latency %d timeout %d\n", p->update_conn_params.conn_int, p->update_conn_params.latency, p->update_conn_params.timeout);
}

static void gattc_cb(esp_gattc_cb_event_t ev, esp_gatt_if_t g, esp_ble_gattc_cb_param_t *p)
{
    switch (ev) {
    case ESP_GATTC_REG_EVT: gif = g; break;
    case ESP_GATTC_OPEN_EVT:
        printf("open status %d conn_id %d mtu %d\n", p->open.status, p->open.conn_id, p->open.mtu);
        if (p->open.status == ESP_GATT_OK) { connected = 1; conn_id_v = p->open.conn_id; esp_ble_gattc_send_mtu_req(g, p->open.conn_id); }
        else opening = 0;
        break;
    case ESP_GATTC_CFG_MTU_EVT: printf("mtu status %d mtu %d\n", p->cfg_mtu.status, p->cfg_mtu.mtu); esp_ble_gattc_search_service(g, p->cfg_mtu.conn_id, NULL); break;
    case ESP_GATTC_SEARCH_RES_EVT: nsvc++; printf("service uuid len %d start %d end %d\n", p->search_res.srvc_id.uuid.len, p->search_res.start_handle, p->search_res.end_handle); break;
    case ESP_GATTC_SEARCH_CMPL_EVT:
        printf("discovery complete status %d, %d services\n", p->search_cmpl.status, nsvc);
        esp_ble_gattc_read_char(g, p->search_cmpl.conn_id, 6, ESP_GATT_AUTH_REQ_NONE);           /* the 0xFFE1 value */
        esp_ble_gattc_register_for_notify(g, peer, 6);
        { uint8_t en[2] = { 1, 0 }; esp_ble_gattc_write_char_descr(g, p->search_cmpl.conn_id, 7, 2, en, ESP_GATT_WRITE_TYPE_RSP, ESP_GATT_AUTH_REQ_NONE); }
        break;
    case ESP_GATTC_READ_CHAR_EVT: printf("read handle %d status %d len %d\n", p->read.handle, p->read.status, p->read.value_len); break;
    case ESP_GATTC_WRITE_DESCR_EVT: printf("CCCD write status %d\n", p->write.status); break;
    case ESP_GATTC_NOTIFY_EVT: { uint32_t v = 0; memcpy(&v, p->notify.value, p->notify.value_len > 4 ? 4 : p->notify.value_len); nnot++; printf("notify handle %d value %lu (#%d)\n", p->notify.handle, (unsigned long)v, nnot); break; }
    case ESP_GATTC_DISCONNECT_EVT: printf("disconnected reason 0x%x\n", p->disconnect.reason); connected = 0; opening = 0; esp_ble_gap_start_scanning(0); break;
    default: break;
    }
}

void app_main(void)
{
    nvs_flash_init();
    esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);
    esp_bt_controller_config_t cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    esp_bt_controller_init(&cfg);
    esp_bt_controller_enable(ESP_BT_MODE_BLE);
    esp_bluedroid_init(); esp_bluedroid_enable();
    esp_ble_gap_register_callback(gap_cb);
    esp_ble_gattc_register_callback(gattc_cb);
    esp_ble_gattc_app_register(0);
    esp_ble_scan_params_t sp = { .scan_type = BLE_SCAN_TYPE_ACTIVE, .own_addr_type = BLE_ADDR_TYPE_PUBLIC, .scan_filter_policy = BLE_SCAN_FILTER_ALLOW_ALL,
                                 .scan_interval = 0x50, .scan_window = 0x50, .scan_duplicate = BLE_SCAN_DUPLICATE_DISABLE };
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_ble_gap_set_scan_params(&sp);
    printf("central ready\n");
    for (int s = 0;; s++) { vTaskDelay(pdMS_TO_TICKS(1000)); printf("t %d s: %s\n", s, connected ? "connected" : (opening ? "connecting" : "scanning")); }
}
