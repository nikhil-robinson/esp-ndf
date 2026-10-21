/* Independent Wi-Fi receiver for esp-ndf's raw 802.11 TX: IDF promiscuous mode on channel CH; every second print
 * all frames, and the frames whose addr2 (transmitter) is the test address 02:4E:44:46:00:01, with rate and RSSI. */
#include <stdio.h>
#include <string.h>
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#ifndef CH
#define CH 6
#endif
static volatile uint32_t all, ours, ours_fcs_ok; static volatile int last_rssi, last_rate, last_len; static uint8_t last_hdr[24];
static const uint8_t test_addr[6] = { 0x02, 0x4E, 0x44, 0x46, 0x00, 0x01 };
static void cb(void *buf, wifi_promiscuous_pkt_type_t type)
{
    const wifi_promiscuous_pkt_t *p = buf; all++;
    if (p->rx_ctrl.sig_len >= 24 && !memcmp(p->payload + 10, test_addr, 6)) {
        ours++; last_rssi = p->rx_ctrl.rssi; last_rate = p->rx_ctrl.rate; last_len = p->rx_ctrl.sig_len; memcpy(last_hdr, p->payload, 24);
    }
}
void app_main(void)
{
    nvs_flash_init();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT(); esp_wifi_init(&cfg);
    esp_wifi_set_mode(WIFI_MODE_NULL); esp_wifi_start();
    wifi_promiscuous_filter_t f = { .filter_mask = WIFI_PROMIS_FILTER_MASK_ALL }; esp_wifi_set_promiscuous_filter(&f);
    esp_wifi_set_promiscuous_rx_cb(cb); esp_wifi_set_promiscuous(true); esp_wifi_set_channel(CH, WIFI_SECOND_CHAN_NONE);
    printf("sniffer ready ch %d\n", CH);
    uint32_t a0 = 0, o0 = 0;
    for (int s = 0; s < 30; s++) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        uint32_t a = all, o = ours;
        printf("t %d: frames/s %lu, ours/s %lu (rssi %d rate %d len %d) hdr", s, a - a0, o - o0, last_rssi, last_rate, last_len);
        for (int k = 0; k < 24; k++) { printf(" %02x", last_hdr[k]); }
        printf("\n");
        a0 = a; o0 = o;
    }
    printf("=== done\n");
}
