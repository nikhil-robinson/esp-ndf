/* ESP-IDF peer for the esp-ndf wireless tests (runs on the BOX-3). One line command in, output lines, then "ok" / "err N".
 * Python side: tools/wl/peer.py. Commands: cmd_ble.c, cmd_wifi.c, cmd_net.c. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "nvs_flash.h"
#include "esp_mac.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "driver/usb_serial_jtag.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "../peer.h"

int hex2bin(const char *h, unsigned char *out, int max)
{
    int n = 0;
    for (; h[0] && h[1] && n < max; h += 2, n++) { char b[3] = { h[0], h[1], 0 }; out[n] = (unsigned char)strtoul(b, 0, 16); }
    return n;
}

static int cmd_mac(int argc, char **argv)
{
    uint8_t m[6]; esp_read_mac(m, ESP_MAC_WIFI_STA);
    printf("wifi_sta %02x:%02x:%02x:%02x:%02x:%02x\n", m[0], m[1], m[2], m[3], m[4], m[5]);
    esp_read_mac(m, ESP_MAC_BT);
    printf("bt %02x:%02x:%02x:%02x:%02x:%02x\n", m[0], m[1], m[2], m[3], m[4], m[5]);
    return 0;
}
static int cmd_reboot(int argc, char **argv) { printf("rebooting\nok\n"); fflush(stdout); vTaskDelay(100 / portTICK_PERIOD_MS); esp_restart(); return 0; }
static int cmd_ver(int argc, char **argv) { printf("idf %s\n", esp_get_idf_version()); return 0; }
static const peer_cmd_t cmds_main[] = { { "mac", cmd_mac }, { "ver", cmd_ver }, { "reboot", cmd_reboot }, { 0 } };

static int run(char *line)
{
    char *argv[12]; int argc = 0;
    for (char *t = strtok(line, " \t\r\n"); t && argc < 12; t = strtok(0, " \t\r\n")) argv[argc++] = t;
    if (!argc) return 1;
    const peer_cmd_t *tabs[] = { cmds_main, cmds_ble, cmds_wifi, cmds_net };
    for (unsigned i = 0; i < 4; i++)
        for (const peer_cmd_t *c = tabs[i]; c->name; c++)
            if (!strcmp(c->name, argv[0])) return c->fn(argc, argv);
    return -100;
}

void app_main(void)
{
    nvs_flash_init();
    esp_netif_init();
    esp_event_loop_create_default();
    usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    usb_serial_jtag_driver_install(&cfg);
    printf("peer ready\n");
    char line[200]; int n = 0;
    for (;;) {
        uint8_t ch;
        if (usb_serial_jtag_read_bytes(&ch, 1, 100 / portTICK_PERIOD_MS) != 1) continue;
        if (ch == '\n' || ch == '\r') {
            line[n] = 0; n = 0;
            if (!line[0]) continue;
            int rc = run(line);
            if (rc == 1) continue;
            if (rc) printf("err %d\n", rc); else printf("ok\n");
            fflush(stdout);
        } else if (n < (int)sizeof line - 1) line[n++] = ch;
    }
}
