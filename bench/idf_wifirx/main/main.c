/* IDF reference receive counter for esp-ndf's own 802.11 TX bring-up (two of my own boards on the lab
 * bench): counts all 802.11 frames per second on channel CH, and separately the frames whose source
 * address (addr2) matches the lab id 02:4E:44:46:00:01, reporting rate and RSSI. Used to confirm the
 * link between the two boards. */
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
static const uint8_t lab_id[6] = { 0x02, 0x4E, 0x44, 0x46, 0x00, 0x01 };
static void cb(void *buf, wifi_promiscuous_pkt_type_t type)
{
    const wifi_promiscuous_pkt_t *p = buf; all++;
    if (p->rx_ctrl.sig_len >= 24 && !memcmp(p->payload + 10, lab_id, 6)) {
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
    printf("receiver ready ch %d\n", CH);
#ifdef DUMP_RX_STATE
    vTaskDelay(pdMS_TO_TICKS(300));                      /* let RX settle and receive some ambient frames */
    printf("=== wifi rx running\n");
    for (uint32_t a = 0x60033000; a < 0x60035000; a += 4) printf("r %08lx %08lx\n", a, *(volatile uint32_t *)a);
    for (uint32_t a = 0x6001c000; a < 0x6001e000; a += 4) printf("r %08lx %08lx\n", a, *(volatile uint32_t *)a);
    for (uint32_t a = 0x60006000; a < 0x60006200; a += 4) printf("r %08lx %08lx\n", a, *(volatile uint32_t *)a);
    /* RX descriptor ring heads live as 20-bit DRAM offsets in 0x60033088..0x98; follow each ring */
    for (uint32_t reg = 0x60033088; reg <= 0x60033098; reg += 4) {
        uint32_t off = *(volatile uint32_t *)reg;
        printf("rxreg %08lx -> off %08lx\n", reg, off);
        uint32_t d = 0x3FC00000u | (off & 0xFFFFF);
        for (int k = 0; k < 4 && d >= 0x3FC80000u && d < 0x3FD00000u; k++) {
            uint32_t w0 = *(volatile uint32_t *)d, w1 = *(volatile uint32_t *)(d + 4),
                     w2 = *(volatile uint32_t *)(d + 8), w3 = *(volatile uint32_t *)(d + 12);
            printf("rxdesc %08lx: %08lx %08lx %08lx %08lx\n", d, w0, w1, w2, w3);
            if (w1 >= 0x3FC80000u && w1 < 0x3FD00000u) {         /* w1 = buffer */
                printf("rxbuf %08lx:", w1);
                for (int j = 0; j < 40; j++) printf(" %02x", *(volatile uint8_t *)(w1 + j));
                printf("\n");
            }
            if (!(w2 & 0xFFFFF)) break;
            d = 0x3FC00000u | (w2 & 0xFFFFF);
        }
    }
    printf("=== done\n");
#endif
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
