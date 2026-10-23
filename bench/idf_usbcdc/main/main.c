/* IDF sibling of examples/usb_cdc: TinyUSB (esp_tinyusb) CDC-ACM with the same protocol, measured by the same host
 * script (tools/usb_cdc_test.py --idf). 'S' + u32 count -> send count bytes (byte k = k & 0xFF); 'U' + u32 count ->
 * receive, check, reply u32 received + u32 errors; other bytes echoed. Takes the PHY at boot (no USB-JTAG console);
 * returns it after 25 s by restarting, as the host test expects the USB-JTAG port back. */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "tinyusb.h"
#include "tusb_cdc_acm.h"

static uint8_t buf[4096];

void app_main(void)
{
    vTaskDelay(pdMS_TO_TICKS(3000));                         /* time for the host to see USB-JTAG and flash */
    const tinyusb_config_t tc = { 0 };
    tinyusb_driver_install(&tc);
    tinyusb_config_cdcacm_t ac = { .usb_dev = TINYUSB_USBDEV_0, .cdc_port = TINYUSB_CDC_ACM_0, .rx_unread_buf_sz = 64 };
    tusb_cdc_acm_init(&ac);
    int64_t t0 = esp_timer_get_time();
    uint32_t mode = 0, left = 0, pos = 0, got = 0, err = 0, have = 0; uint8_t hdr[5], in[64];
    while (esp_timer_get_time() - t0 < 25000000) {
        if (mode == 1) {
            if (!left) { mode = 0; tinyusb_cdcacm_write_flush(TINYUSB_CDC_ACM_0, 0); continue; }
            uint32_t n = left < sizeof buf ? left : sizeof buf;
            for (uint32_t k = 0; k < n; k++) buf[k] = (uint8_t)(pos + k);
            size_t q = tinyusb_cdcacm_write_queue(TINYUSB_CDC_ACM_0, buf, n);
            tinyusb_cdcacm_write_flush(TINYUSB_CDC_ACM_0, 0);
            pos += q; left -= q;
            if (!q) vTaskDelay(1);
            continue;
        }
        size_t n = 0;
        if (tinyusb_cdcacm_read(TINYUSB_CDC_ACM_0, in, sizeof in, &n) != ESP_OK || !n) { vTaskDelay(1); continue; }
        for (size_t k = 0; k < n; k++) {
            if (mode == 2) {
                if (in[k] != (uint8_t)got) err++;
                if (++got == left) { uint32_t r[2] = { got, err }; mode = 0; tinyusb_cdcacm_write_queue(TINYUSB_CDC_ACM_0, (uint8_t *)r, 8); tinyusb_cdcacm_write_flush(TINYUSB_CDC_ACM_0, 0); }
                continue;
            }
            if (have || in[k] == 'S' || in[k] == 'U') {
                hdr[have++] = in[k];
                if (have == 5) { uint32_t c; memcpy(&c, hdr + 1, 4); have = 0; if (hdr[0] == 'S') { mode = 1; left = c; pos = 0; } else { mode = 2; left = c; got = err = 0; } }
                continue;
            }
            tinyusb_cdcacm_write_queue(TINYUSB_CDC_ACM_0, &in[k], 1); tinyusb_cdcacm_write_flush(TINYUSB_CDC_ACM_0, 0);
        }
    }
    esp_restart();
}
