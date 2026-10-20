/* Hardware BLE receive, step 1: what the vendor controller leaves in the BLE core while it scans.
 * Prints the ROM's BT memory ranges, the exchange-memory base, and the BLE core registers
 * (0x60011000) at idle and during a continuous passive scan on channel 37. */
#include <stdio.h>
#include <string.h>
#include "nvs_flash.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_ble_api.h"
#include "rom/rom_layout.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_cpu.h"
#include "rf_regi2c.h"
#include "rf_init.h"
#ifndef ONTO
#define ONTO 1
#endif
extern void coex_pti_v2(void);
extern void bt_bb_v2_init_cmplx(int);
#include "rf_pll.h"
#include "esp_rom_sys.h"
void rf_delay_us(uint32_t us) { esp_rom_delay_us(us); }

extern void r_emi_em_base_reg_show(void);
static volatile int hits;
static void cb(esp_gap_ble_cb_event_t ev, esp_ble_gap_cb_param_t *p)
{
    if (ev == ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT) esp_ble_gap_start_scanning(0);
    if (ev == ESP_GAP_BLE_SCAN_RESULT_EVT && p->scan_rst.search_evt == ESP_GAP_SEARCH_INQ_RES_EVT) hits++;
}

static uint32_t snap[0x400];
static void dump(const char *tag)
{
    printf("== %s\n", tag);
    for (int k = 0; k < 0x400; k++) {
        uint32_t v = *(volatile uint32_t *)(0x60011000 + 4 * k);
        if (v) printf("bt %03x %08lx\n", 4 * k, (unsigned long)v);
        snap[k] = v;
    }
}

#define CORE(o) (*(volatile uint32_t *)(0x60031000 + (o)))
static uint32_t em_map(uint32_t off)               /* EM offset -> DRAM: the page register with the largest base <= off */
{
    uint32_t best = 0, bo = 0;
    for (int k = 0; k < 56; k++) {
        uint32_t v = *(volatile uint32_t *)(0x60031204 + 4 * k + (k > 47 ? 28 : 0)), o = (v >> 18) << 2;
        if ((v & 0x3FFFF) && o <= off && o >= bo) { bo = o; best = ((v & 0x3FFFF) << 2) | 0x3FC00000; }
    }
    return best + off - bo;
}
#define EM16(off) (*(volatile uint16_t *)em_map(off))
static uint32_t clkn(void)                          /* SLOTCLK (v9 at 0x1C): sample, wait, half-slots [27:0] */
{
    CORE(0x1C) = (CORE(0x1C) & 0x7FFFFFFF) | 0x80000000;
    while (CORE(0x1C) & 0x80000000) {}
    return CORE(0x1C) & 0x0FFFFFFF;
}

/* our own event on the vendor-configured core: ET entry 0 -> the vendor's scan CS, RX ring at 0x1000 */
static void kick_test(void)
{
    esp_rom_printf("== kick: et0 %04x %04x %04x %04x %04x  rxptr %08lx\n", EM16(0), EM16(2), EM16(4), EM16(6), EM16(8), (unsigned long)CORE(0x24));
    for (uint32_t d = 0x1000; d < 0x10C8; d += 0x14) EM16(d + 4) = 0;           /* clear RXPH so a new packet shows */
    static char out[2048]; int no = 0;
    portDISABLE_INTERRUPTS();
    uint32_t intcntl = CORE(0x0C);
    CORE(0x0C) = 0;                                 /* the vendor ISR stays out: we poll */
    for (int r = 0; r < 8; r++) {
        uint32_t ck = clkn();
        uint16_t w0 = EM16(0);
        EM16(0) = w0 & ~0x38;                       /* status: standby */
        EM16(2) = (ck + 8) & 0xFFFF; EM16(4) = ((ck + 8) >> 16) & 0xFFF; EM16(6) = 0;
        CORE(0x24) = 0x1000;
        CORE(0x100) = 0x80000000;                   /* START_ACT, entry 0 */
        uint32_t t0 = esp_cpu_get_cycle_count(), hit = 0;
        while (esp_cpu_get_cycle_count() - t0 < 240000u * 40) {   /* 40 ms */
            for (uint32_t d = 0x1000; d < 0x10C8; d += 0x14) if (EM16(d + 4)) hit = d;
            if (hit) break;
        }
        no += snprintf(out + no, sizeof out - no, "try %d clkn %07lx->%07lx et0 %04x st %lu rx %08lx int %08lx 2d8 %08lx", r, (unsigned long)ck, (unsigned long)clkn(),
               EM16(0), (unsigned long)((EM16(0) >> 3) & 7), (unsigned long)CORE(0x24), (unsigned long)CORE(0x14), (unsigned long)CORE(0x2D8));
        CORE(0x18) = 0xFFFFFFFF;
        no += snprintf(out + no, sizeof out - no, " d0:");
        for (int k = 0; k < 20; k += 2) no += snprintf(out + no, sizeof out - no, " %04x", EM16(0x1000 + k));
        if (hit) no += snprintf(out + no, sizeof out - no, " RX desc %04lx ph %04x chass %04x data %04x\n", (unsigned long)hit, EM16(hit + 4), EM16(hit + 6), EM16(hit + 0x12));
        else no += snprintf(out + no, sizeof out - no, " no rx\n");
    }
    CORE(0x18) = 0xFFFFFFFF;
    portENABLE_INTERRUPTS();
    esp_rom_printf("intcntl was %08lx\n", (unsigned long)intcntl);
    esp_rom_printf("%s", out);
}

/* full idle state after a vendor scan, for the bare-metal replay (tools/gen_ble_state.py) */
static void state_dump(void)
{
    static const struct { uint32_t base, len; } blk[] = {
        { 0x60011000, 0x100 }, { 0x60031000, 0x400 }, { 0x60006000, 0x200 }, { 0x60005000, 0x1000 }, { 0x6001C000, 0x2000 },
        { 0x6000E000, 0x200 }, { 0x60026000, 0x100 }, { 0x60008000, 0x140 }, { 0x600C0000, 0x100 }, { 0x60033000, 0x3000 },
    };
    printf("== state\n");
    for (unsigned b = 0; b < sizeof blk / sizeof blk[0]; b++)
        for (uint32_t a = blk[b].base; a < blk[b].base + blk[b].len; a += 4)
            printf("r %08lx %08lx\n", (unsigned long)a, (unsigned long)*(volatile uint32_t *)a);
    static const uint8_t ab[] = { 0x61, 0x62, 0x63, 0x66, 0x67, 0x68, 0x69, 0x6A, 0x6B, 0x6C, 0x6D };
    for (unsigned b = 0; b < sizeof ab; b++)
        for (int r = 0; r < 32; r++) printf("a %02x %02x %02x\n", ab[b], r, rf_regi2c_read(ab[b], r));
    uint32_t lo = 0xFFFFFFFF, hi = 0;
    for (int k = 0; k < 56; k++) {
        uint32_t v = *(volatile uint32_t *)(0x60031204 + 4 * k + (k > 47 ? 28 : 0)), a = ((v & 0x3FFFF) << 2) | 0x3FC00000;
        if (v & 0x3FFFF) { printf("seg %d off %04lx addr %08lx\n", k, (unsigned long)((v >> 18) << 2), (unsigned long)a); if (a < lo) lo = a; if (a > hi) hi = a; }
    }
    hi += 0x400;
    for (uint32_t a = lo; a < hi; a += 32) {
        printf("e %08lx", (unsigned long)a);
        for (int k = 0; k < 8; k++) printf(" %08lx", (unsigned long)((volatile uint32_t *)a)[k]);
        printf("\n");
    }
    printf("== state end\n");
}

void app_main(void)
{
    const ets_rom_layout_t *l = ets_rom_layout_p;
    printf("rom btdm data %p-%p bss %p-%p romdata %p-%p if data %p-%p if bss %p-%p\n", l->data_start_btdm, l->data_end_btdm,
           l->bss_start_btdm, l->bss_end_btdm, l->data_start_btdm_rom, l->data_end_btdm_rom, l->data_start_interface_btdm,
           l->data_end_interface_btdm, l->bss_start_interface_btdm, l->bss_end_interface_btdm);
    nvs_flash_init();
    esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);
    esp_bt_controller_config_t cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    esp_bt_controller_init(&cfg);
    esp_bt_controller_enable(ESP_BT_MODE_BLE);
    esp_bluedroid_init();
    esp_bluedroid_enable();
    r_emi_em_base_reg_show();
    dump("idle");
    esp_ble_gap_register_callback(cb);
    esp_ble_scan_params_t sp = { .scan_type = BLE_SCAN_TYPE_PASSIVE, .own_addr_type = BLE_ADDR_TYPE_PUBLIC,
                                 .scan_filter_policy = BLE_SCAN_FILTER_ALLOW_ALL, .scan_interval = 0x4000, .scan_window = 0x4000,
                                 .scan_duplicate = BLE_SCAN_DUPLICATE_DISABLE };
    esp_ble_gap_set_scan_params(&sp);
    vTaskDelay(pdMS_TO_TICKS(1000));
    dump("scanning");
    for (int r = 0; r < 3; r++) {                    /* which registers move while scanning */
        vTaskDelay(pdMS_TO_TICKS(50));
        for (int k = 0; k < 0x400; k++) {
            uint32_t v = *(volatile uint32_t *)(0x60011000 + 4 * k);
            if (v != snap[k]) printf("moves %03x %08lx -> %08lx\n", 4 * k, (unsigned long)snap[k], (unsigned long)v);
            snap[k] = v;
        }
    }
    printf("hits %d\n", hits);
    esp_ble_gap_stop_scanning();
    vTaskDelay(pdMS_TO_TICKS(200));
    CORE(0x0C) = 0;                                 /* vendor ISR out: the state stays as the last scan left it */
    vTaskDelay(pdMS_TO_TICKS(50));
    state_dump();
    vTaskDelay(pdMS_TO_TICKS(500));
    vTaskSuspendAll();                              /* vendor frozen from here: no tasks, core interrupts masked */
    kick_test();
    esp_rom_printf("== our PHY init on top of the vendor BLE state\n");
    static const struct { uint32_t base, len; } rb[] = {
        { 0x60033000, 0x3000 }, { 0x6001C000, 0x2000 }, { 0x60006000, 0x200 }, { 0x6000E000, 0x200 }, { 0x60031400, 0x400 },
        { 0x60011000, 0x1000 },
    };
    static uint32_t big[(0x3000 + 0x2000 + 0x200 + 0x200 + 0x400 + 0x1000) / 4];
    { uint32_t *q = big; for (unsigned b = 0; b < sizeof rb / sizeof rb[0]; b++) for (uint32_t a = rb[b].base; a < rb[b].base + rb[b].len; a += 4, q++) if ((ONTO >> (b + 3)) & 1) *q = *(volatile uint32_t *)a; }
    static uint32_t sm[64], sc[0x100];
    uint32_t sclk = *(volatile uint32_t *)0x60026014, srst = *(volatile uint32_t *)0x60026018;
    for (int k = 0; k < 64; k++) sm[k] = *(volatile uint32_t *)(0x60011000 + 4 * k);
    uint32_t r1 = *(volatile uint32_t *)0x60026018;
    uint32_t rk = 0xFFFF;
    for (int k = 0; k < 0x100; k++) { sc[k] = CORE(4 * k); if (rk == 0xFFFF && *(volatile uint32_t *)0x60026018) rk = 4 * k; }
    uint32_t r2 = *(volatile uint32_t *)0x60026018;
    portDISABLE_INTERRUPTS();
    uint32_t r3 = *(volatile uint32_t *)0x60026018;
    esp_rom_printf("rst after modem reads %08lx, first core read setting it %04lx, after core reads %08lx, after irq off %08lx\n", (unsigned long)r1, (unsigned long)rk, (unsigned long)r2, (unsigned long)r3);
#if ONTO & 2
                                                    /* no reset: checks the restore itself */
#elif ONTO & 1
    rf_power_on();                                  /* modem reset only */
#else
    rf_init_replay();
    rf_pll_result_t pr; rf_pll_set_khz(2402000, &pr);
    rf_rx_on();
#endif
    uint32_t clk = *(volatile uint32_t *)0x60026014, rst = *(volatile uint32_t *)0x60026018;
    *(volatile uint32_t *)0x60026014 = sclk; *(volatile uint32_t *)0x60026018 = srst;
    for (int k = 0; k < 64; k++) *(volatile uint32_t *)(0x60011000 + 4 * k) = sm[k];
    for (int k = 1; k < 0x100; k++) {               /* same rule as the bare-metal replay */
        uint32_t o = 4 * k;
        if (o == 0x10 || o == 0x14 || o == 0x18 || o == 0x1C || o == 0x20 || o == 0x24 || (o >= 0x100 && o < 0x120)) continue;
        if (o >= 0x200 && o < 0x2C0) { CORE(o) = sc[k]; continue; }    /* EM map: same DRAM */
        if (sc[k] || o < 0x100) CORE(o) = sc[k];
    }
    CORE(0x0C) = 0;
    CORE(0) = sc[0];
#if ONTO >= 4
    { uint32_t *q = big; int nw = 0;
      for (unsigned b = 0; b < sizeof rb / sizeof rb[0]; b++)
          for (uint32_t a = rb[b].base; a < rb[b].base + rb[b].len; a += 4, q++)
              if ((ONTO >> (b + 3)) & 1 && *(volatile uint32_t *)a != *q) { *(volatile uint32_t *)a = *q; nw++; }
      (void)nw; }
#endif
#if ONTO == 5
    coex_pti_v2();
#elif ONTO == 7
    bt_bb_v2_init_cmplx(0);
#endif
    portENABLE_INTERRUPTS();
    esp_rom_printf("before: clk %08lx rst %08lx; after our init: syscon clk %08lx rst %08lx, map0 %08lx\n", (unsigned long)sclk, (unsigned long)srst, (unsigned long)clk, (unsigned long)rst, (unsigned long)CORE(0x204));
    kick_test();
    esp_rom_printf("=== done\n");
    for (;;) {}
    printf("== core 0x60031000\n");
    for (int k = 0; k < 0x100; k++) {
        uint32_t v = *(volatile uint32_t *)(0x60031000 + 4 * k);
        if (v) printf("core %03x %08lx\n", 4 * k, (unsigned long)v);
    }
    printf("== em map\n");                          /* 56 page registers: [31:18] check, [17:0] DRAM word address */
    for (int k = 0; k < 56; k++) {
        uint32_t v = *(volatile uint32_t *)(0x60031204 + 4 * k + (k > 47 ? 28 : 0));
        if (v) printf("em %2d %08lx -> %08lx\n", k, (unsigned long)v, (unsigned long)(((v & 0x3FFFF) << 2) | 0x3FC00000));
    }
    portDISABLE_INTERRUPTS();
    static uint32_t em[0x800];                       /* 8 KB from the lowest mapped segment */
    uint32_t lo = 0xFFFFFFFF, offs[56], addr[56]; int ns = 0;
    for (int k = 0; k < 56; k++) {
        uint32_t v = *(volatile uint32_t *)(0x60031204 + 4 * k + (k > 47 ? 28 : 0));
        if (v & 0x3FFFF) { offs[ns] = (v >> 18) << 2; addr[ns] = ((v & 0x3FFFF) << 2) | 0x3FC00000; if (addr[ns] < lo) lo = addr[ns]; ns++; }
    }
    for (int k = 0; k < 0x800; k++) em[k] = ((volatile uint32_t *)lo)[k];
    static uint32_t core[0x100];
    for (int k = 0; k < 0x100; k++) core[k] = *(volatile uint32_t *)(0x60031000 + 4 * k);
    portENABLE_INTERRUPTS();
    for (int k = 0; k < ns; k++) printf("seg off %04lx addr %08lx\n", (unsigned long)offs[k], (unsigned long)addr[k]);
    printf("== emdump base %08lx\n", (unsigned long)lo);
    for (int k = 0; k < 0x800; k += 8)
        printf("m %08lx %08lx %08lx %08lx %08lx %08lx %08lx %08lx %08lx\n", (unsigned long)(lo + 4 * k), (unsigned long)em[k], (unsigned long)em[k + 1],
               (unsigned long)em[k + 2], (unsigned long)em[k + 3], (unsigned long)em[k + 4], (unsigned long)em[k + 5], (unsigned long)em[k + 6], (unsigned long)em[k + 7]);
    printf("== coresnap\n");
    for (int k = 0; k < 0x100; k++) if (core[k]) printf("c %03x %08lx\n", 4 * k, (unsigned long)core[k]);
    printf("sys 60026014 %08lx 60026018 %08lx 600c0018 %08lx 600c001c %08lx\n", (unsigned long)*(volatile uint32_t *)0x60026014,
           (unsigned long)*(volatile uint32_t *)0x60026018, (unsigned long)*(volatile uint32_t *)0x600c0018, (unsigned long)*(volatile uint32_t *)0x600c001c);
    printf("=== done\n");
    for (;;) vTaskDelay(1000);
}
