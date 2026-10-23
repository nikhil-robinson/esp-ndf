/* IDF sibling of examples/usb_cdc: TinyUSB (esp_tinyusb) CDC-ACM with the same protocol, measured by the same host
 * script (tools/usb_cdc_test.py --idf). 'S' + u32 count -> send count bytes (byte k = k & 0xFF); 'U' + u32 count ->
 * receive, check, reply u32 received + u32 errors; other bytes echoed. Takes the PHY 3 s after boot (no USB-JTAG
 * console); after 25 s it uninstalls TinyUSB, hands the PHY back to USB Serial/JTAG and parks (esp_restart would not:
 * the PHY selection is in the RTC domain, so the board would come back on OTG). */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "tinyusb.h"
#include "tusb_cdc_acm.h"
#include "soc/rtc_cntl_reg.h"

static volatile uint32_t spin;                            /* core-0 load probe: lowest-priority counter */
static void spin_task(void *arg) { for (;;) spin++; }
#define RESULT ((volatile uint32_t *)0x50001000u)          /* RTC slow memory: read back with esptool read_mem */
static uint8_t buf[7936];                                /* 31 x 256: pattern continuous across chunks */

static void test_task(void *arg)
{
    for (uint32_t k = 0; k < sizeof buf; k++) buf[k] = (uint8_t)k;   /* pattern source, made once (as ours) */
    vTaskDelay(pdMS_TO_TICKS(2000));                         /* time for the host to see USB-JTAG and flash */
    uint32_t b0 = spin; vTaskDelay(pdMS_TO_TICKS(1000)); uint32_t base = spin - b0;   /* core-0 idle rate per second */
    uint32_t s0 = 0, t_s0 = 0;
    const tinyusb_config_t tc = { 0 };
    tinyusb_driver_install(&tc);
    tinyusb_config_cdcacm_t ac = { .usb_dev = TINYUSB_USBDEV_0, .cdc_port = TINYUSB_CDC_ACM_0, .rx_unread_buf_sz = 512 };
    tusb_cdc_acm_init(&ac);
    int64_t t0 = esp_timer_get_time();
    uint32_t mode = 0, left = 0, pos = 0, got = 0, err = 0, have = 0; uint8_t hdr[5]; static uint8_t in[512];
    while (esp_timer_get_time() - t0 < 25000000) {
        if (mode == 1) {
            if (!left) { mode = 0; tinyusb_cdcacm_write_flush(TINYUSB_CDC_ACM_0, 0); continue; }
            uint32_t n = left < 7680 ? left : 7680;          /* from buf + pos % 256: a partial queue keeps the pattern */
            size_t q = tinyusb_cdcacm_write_queue(TINYUSB_CDC_ACM_0, buf + (pos & 255), n);
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
                if (++got == left) { uint32_t r[2] = { got, err }; mode = 0;
                    uint32_t ms = (uint32_t)(esp_timer_get_time() / 1000) - t_s0, sp = spin - s0;   /* download + upload span */
                    RESULT[1] = base; RESULT[2] = sp; RESULT[3] = ms; RESULT[0] = 0x49444631u; tinyusb_cdcacm_write_queue(TINYUSB_CDC_ACM_0, (uint8_t *)r, 8); tinyusb_cdcacm_write_flush(TINYUSB_CDC_ACM_0, 0); }
                continue;
            }
            if (have || in[k] == 'S' || in[k] == 'U') {
                hdr[have++] = in[k];
                if (have == 5) { uint32_t c; memcpy(&c, hdr + 1, 4); have = 0; if (hdr[0] == 'S') { mode = 1; left = c; pos = 0; s0 = spin; t_s0 = (uint32_t)(esp_timer_get_time() / 1000); } else { mode = 2; left = c; got = err = 0; } }
                continue;
            }
            tinyusb_cdcacm_write_queue(TINYUSB_CDC_ACM_0, &in[k], 1); tinyusb_cdcacm_write_flush(TINYUSB_CDC_ACM_0, 0);
        }
    }
    tinyusb_driver_uninstall();
    REG_CLR_BIT(RTC_CNTL_USB_CONF_REG, RTC_CNTL_SW_USB_PHY_SEL | RTC_CNTL_SW_HW_USB_PHY_SEL);   /* PHY back to USB Serial/JTAG */
    for (;;) vTaskDelay(pdMS_TO_TICKS(1000));
}

void app_main(void)
{
    RESULT[0] = 0;
    xTaskCreatePinnedToCore(spin_task, "spin", 2048, NULL, 1, NULL, 0);          /* core 0 with the TinyUSB task (24) */
    xTaskCreatePinnedToCore(test_task, "usbtest", 8192, NULL, 23, NULL, 1);   /* core 1, priority 23 (TinyUSB: 24 on core 0) */
}
