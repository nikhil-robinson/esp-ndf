/* Register snapshot for the standalone-PHY work (docs/PHY_STANDALONE.md):
 * always-clocked blocks before Wi-Fi, then every modem block after
 * esp_wifi_start(). Output: "@stage" then "addr value" lines. */
#include <stdio.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "esp_phy_init.h"
#include "esp_private/wifi.h"
void esp_wifi_bt_power_domain_on(void);
#ifndef PHY_ONLY
#define PHY_ONLY 0                                    /* 1: PHY bring-up only, no Wi-Fi stack */
#endif

typedef struct { const char *name; uint32_t base, len; int modem; } blk_t;
static const blk_t blks[] = {
    { "SYSCON",    0x60026000, 0x100, 0 },
    { "RTC_CNTL",  0x60008000, 0x140, 0 },
    { "SENSITIVE", 0x600C1000, 0x40,  0 },
    { "FE2",       0x60005000, 0x1000, 1 },
    { "FE",        0x60006000, 0x1000, 1 },
    { "AGC_NRX_BB",0x6001C000, 0x2000, 1 },
    { "MAC",       0x60033000, 0x3000, 1 },
    { "I2C_MST",   0x6000E000, 0x200, 1 },
};

static void dump(const char *stage, int modem)
{
    printf("@%s\n", stage);
    for (unsigned b = 0; b < sizeof blks / sizeof blks[0]; b++) {
        if (blks[b].modem && !modem) continue;
        for (uint32_t a = blks[b].base; a < blks[b].base + blks[b].len; a += 4)
            printf("%08lx %08lx\n", (unsigned long)a, (unsigned long)*(volatile uint32_t *)a);
        vTaskDelay(1);
    }
}

void app_main(void)
{
    vTaskDelay(pdMS_TO_TICKS(1500));
    dump("before", 0);
    nvs_flash_init();
#if PHY_ONLY
    esp_wifi_bt_power_domain_on();
    *(volatile uint32_t *)0x60026014 |= 0x0078078F | 0x00400000;   /* modem clocks on, as esp_phy_enable does first */
    dump("pre_init", 1);
    esp_phy_enable(PHY_MODEM_WIFI);
    vTaskDelay(pdMS_TO_TICKS(100));
    dump("phy_only", 1);
    vTaskDelay(pdMS_TO_TICKS(100));
    dump("phy_only2", 1);
    /* the vendor receive preparation, as esp_sdr does it */
    {
        extern void stop_tx_tone(uint32_t), rom_pbus_workmode(void), rom_pbus_xpd_tx_off(void);
        extern void rom_pbus_xpd_rx_on(uint32_t), rom_set_rxclk_en(uint32_t);
        extern void set_chanfreq(uint32_t, uint32_t), force_rx_gain(uint32_t, uint32_t, uint32_t);
        *(volatile uint32_t *)0x60026014 |= 0x00400000;
        set_chanfreq(2437, 0);
        stop_tx_tone(1);
        dump("after_stop_tone", 1);
        rom_pbus_workmode();
        rom_pbus_xpd_tx_off();
        rom_pbus_xpd_rx_on(1);
        rom_set_rxclk_en(1);
        force_rx_gain(1, 40, 0);
        vTaskDelay(1);
        dump("rx_ready", 1);
    }
    printf("=== done\n");
    return;
#endif
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);
    dump("after_init", 0);
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_start();
    vTaskDelay(pdMS_TO_TICKS(500));
    dump("after_start", 1);
    printf("=== done\n");
}
/* librftest's test shell hook (unused here) */
int cmd_parse(char *cmd, char *name, int *argc, char **argv) { (void)cmd; (void)name; (void)argc; (void)argv; return -1; }
