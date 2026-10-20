/* Which vendor BT bring-up step leaves the hidden state hardware BLE RX needs? VSTEP (x10 by CMake):
 * 0 nothing, 10 esp_bt_controller_init, 20 + enable, 30 + bluedroid + 1 s scan. Then exactly the
 * bare-metal sequence (examples/ble_hwrx): our PHY init, generated BLE state, kicks. */
#include <stdio.h>
#include <string.h>
#include "nvs_flash.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_ble_api.h"
#include "esp_rom_sys.h"
#include "esp_cpu.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "rf_regi2c.h"
#include "rf_pll.h"
#include "rf_init.h"
#include "ble_state.h"
#include "esp_phy_init.h"
void esp_wifi_bt_power_domain_on(void);
extern void rom_i2cmst_reg_init(void), rom_fe_reg_init(void), rom_agc_reg_init(void), rom_bb_reg_init(void), rom_phy_reg_init(void);
void rf_delay_us(uint32_t us) { esp_rom_delay_us(us); }
#define R32(a)  (*(volatile uint32_t *)(a))
#define CORE(o) R32(0x60031000u + (o))
static uint32_t em_buf[BLE_EM_WORDS];
static uint32_t em_addr(uint32_t off)
{
    unsigned b = 0;
    for (unsigned k = 0; k < sizeof ble_em_segs / sizeof ble_em_segs[0]; k++)
        if (ble_em_segs[k].em_off <= off && ble_em_segs[k].em_off >= ble_em_segs[b].em_off) b = k;
    return (uint32_t)em_buf + ble_em_segs[b].pos + off - ble_em_segs[b].em_off;
}
#define EM16(off) (*(volatile uint16_t *)em_addr(off))
static uint32_t clkn(void)
{
    CORE(0x1C) = (CORE(0x1C) & 0x7FFFFFFFu) | 0x80000000u;
    while (CORE(0x1C) & 0x80000000u) {}
    return CORE(0x1C) & 0x0FFFFFFFu;
}
static void cb(esp_gap_ble_cb_event_t ev, esp_ble_gap_cb_param_t *p) { (void)p; if (ev == ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT) esp_ble_gap_start_scanning(0); }

void app_main(void)
{
    vTaskDelay(pdMS_TO_TICKS(500));
    nvs_flash_init();
#if VSTEP >= 60
    {   /* vendor libphy pieces before our init (as bench/idf_rfrx VFUNC) */
        esp_wifi_bt_power_domain_on();
        rf_power_on();
        *(volatile uint32_t *)0x60026014 |= 0x00400000;
        extern void phy_get_romfunc_addr(void), register_chipv7_phy_init_param(const void *), rf_init(void), bb_init(void);
        phy_get_romfunc_addr();
        void (**tab)(void) = *(void (***)(void))0x3FCEF3D4;
        tab[0x204 / 4](); tab[0x234 / 4]();
        register_chipv7_phy_init_param(esp_phy_get_init_data());
        if ((VSTEP - 60) & 1) rf_init();
        if ((VSTEP - 60) & 2) bb_init();
#ifdef FREQDUMP
        uint32_t c4 = R32(0x6000E0C4);
        for (int a = 0; a < 0x60; a++) {
            R32(0x6000E0C4) = (c4 & ~0xFFu) | a;
            uint32_t v48 = R32(0x6000E148), v4c = R32(0x6000E14C);
            R32(0x6000E0C4) |= 1u << 10; R32(0x6000E0C4) &= ~(1u << 10);
            esp_rom_printf("fm %02x %08x %08x after-b10 %08x %08x\n", a, (unsigned)v48, (unsigned)v4c, (unsigned)R32(0x6000E148), (unsigned)R32(0x6000E14C));
        }
        R32(0x6000E0C4) = c4;
#endif
    }
#elif VSTEP >= 40
    esp_wifi_bt_power_domain_on();
    rf_power_on();
    *(volatile uint32_t *)0x60026014 |= 0x00400000;
    if ((VSTEP - 40) & 1)  rom_i2cmst_reg_init();
    if ((VSTEP - 40) & 2)  rom_fe_reg_init();
    if ((VSTEP - 40) & 4)  rom_agc_reg_init();
    if ((VSTEP - 40) & 8)  rom_bb_reg_init();
    if ((VSTEP - 40) & 16) rom_phy_reg_init();
#elif VSTEP == 21 || VSTEP == 23
    esp_wifi_bt_power_domain_on();
    esp_phy_enable(VSTEP == 21 ? PHY_MODEM_BT : PHY_MODEM_WIFI);
#elif VSTEP >= 10 && VSTEP < 40
    esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);
    esp_bt_controller_config_t cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    esp_bt_controller_init(&cfg);
#endif
#if VSTEP >= 20 && VSTEP < 21 || VSTEP >= 30 && VSTEP < 40
    esp_bt_controller_enable(ESP_BT_MODE_BLE);
#endif
#if VSTEP >= 30 && VSTEP < 40
    esp_bluedroid_init(); esp_bluedroid_enable();
    esp_ble_gap_register_callback(cb);
    esp_ble_scan_params_t sp = { .scan_type = BLE_SCAN_TYPE_PASSIVE, .own_addr_type = BLE_ADDR_TYPE_PUBLIC, .scan_filter_policy = BLE_SCAN_FILTER_ALLOW_ALL,
                                 .scan_interval = 0x4000, .scan_window = 0x4000, .scan_duplicate = BLE_SCAN_DUPLICATE_DISABLE };
    esp_ble_gap_set_scan_params(&sp);
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_ble_gap_stop_scanning();
    vTaskDelay(pdMS_TO_TICKS(200));
#endif
#if VSTEP >= 20 && VSTEP < 21 || VSTEP >= 30 && VSTEP < 40
    CORE(0x0C) = 0;
#endif
    vTaskDelay(pdMS_TO_TICKS(100));
    vTaskSuspendAll();
    portDISABLE_INTERRUPTS();
    rf_init_replay();
    rf_pll_result_t r; rf_pll_set_khz(2402000, &r);
    rf_rx_on();
    R32(0x60026014) = 0xFFFFDFDFu; R32(0x60026018) = 0;
    R32(0x6000E0C4) |= 1u << 25;
    for (unsigned k = 0; k < sizeof ble_vendor_phy / sizeof ble_vendor_phy[0]; k++) R32(ble_vendor_phy[k].addr) = ble_vendor_phy[k].val;
    R32(0x6000E0C4) = 0xC142287Cu & ~(1u << 25);
    for (int k = 0; k < 64; k++) R32(0x60011000u + 4 * k) = ble_modem_regs[k];
    memcpy(em_buf, ble_em_image, sizeof em_buf);
    for (unsigned k = 0; k < sizeof ble_em_segs / sizeof ble_em_segs[0]; k++) {
        unsigned i = ble_em_segs[k].idx; uint32_t a = (uint32_t)em_buf + ble_em_segs[k].pos;
        R32(0x60031204u + 4 * i + (i > 47 ? 28 : 0)) = ((uint32_t)ble_em_segs[k].em_off >> 2) << 18 | ((a >> 2) & 0x3FFFF);
    }
    for (unsigned k = 0; k < sizeof ble_core_regs / sizeof ble_core_regs[0]; k++) CORE(ble_core_regs[k].off) = ble_core_regs[k].val;
    CORE(0x0C) = 0;
    CORE(0x00) = ble_rwblecntl;
    int hits = 0;
    for (int t = 0; t < 10; t++) {
        for (uint32_t d = 0x1000; d < 0x10C8; d += 0x14) { EM16(d) &= 0x7FFF; EM16(d + 4) = 0; }
        uint32_t ck = clkn();
        EM16(0) = EM16(0) & ~0x38u;
        EM16(2) = (ck + 8) & 0xFFFF; EM16(4) = ((ck + 8) >> 16) & 0xFFF; EM16(6) = 0;
        CORE(0x24) = 0x1000; CORE(0x18) = 0xFFFFFFFFu; CORE(0x100) = 0x80000000u;
        uint32_t t0 = esp_cpu_get_cycle_count(), hit = 0;
        while (!hit && esp_cpu_get_cycle_count() - t0 < 240000u * 40)
            for (uint32_t d = 0x1000; d < 0x10C8; d += 0x14) if (EM16(d) & 0x8000) { hit = d; break; }
        hits += hit != 0;
    }
    portENABLE_INTERRUPTS();
    esp_rom_printf("VSTEP %d: hits %d/10 modem b0 %08x\n=== done\n", VSTEP, hits, (unsigned)R32(0x600110B0));
    for (;;) {}
}
