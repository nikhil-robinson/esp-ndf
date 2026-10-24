/* IDF sibling of examples/ulp_test: the same ULP binary through ulp_riscv_load_binary + ulp_riscv_run. */
#include <stdio.h>
#include "esp_cpu.h"
#include "ulp_riscv.h"
#include "soc/rtc_cntl_reg.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ulp_count.h"
#include "ulp_main.h"
extern const uint8_t ulp_main_bin_start[] asm("_binary_ulp_main_bin_start");
extern const uint8_t ulp_main_bin_end[] asm("_binary_ulp_main_bin_end");
#define M(o) (*(volatile uint32_t *)(0x50000000u + (o)))
void app_main(void)
{
    vTaskDelay(100);
    uint32_t t = esp_cpu_get_cycle_count();
    ulp_riscv_load_binary(ulp_count_bin, sizeof ulp_count_bin);
    M(0x208) = 41;
    ulp_set_wakeup_period(0, 10000);
    ulp_riscv_run();
    uint32_t tl = esp_cpu_get_cycle_count() - t;
    if (0) printf("  COCPU_CTRL %08lx ULP_TIMER %08lx ULP_CTRL %08lx TIMER1 %08lx CLK_CONF %08lx OPTIONS0 %08lx\n",
           (unsigned long)REG_READ(RTC_CNTL_COCPU_CTRL_REG), (unsigned long)REG_READ(RTC_CNTL_ULP_CP_TIMER_REG),
           (unsigned long)REG_READ(RTC_CNTL_ULP_CP_CTRL_REG), (unsigned long)REG_READ(RTC_CNTL_ULP_CP_TIMER_1_REG),
           (unsigned long)REG_READ(RTC_CNTL_CLK_CONF_REG), (unsigned long)REG_READ(RTC_CNTL_OPTIONS0_REG));
    uint32_t c0 = M(0x200); vTaskDelay(pdMS_TO_TICKS(1000)); uint32_t c1 = M(0x200);
    printf("idf ULP-RISC-V: load+start %lu us, %lu runs in 1 s, magic %d, mailbox %d\n", (unsigned long)(tl / 240), (unsigned long)(c1 - c0),
           M(0x204) == 0x554C5021u, M(0x20C) == 42);
    ulp_riscv_halt();
    vTaskDelay(10);
    ulp_riscv_load_binary(ulp_main_bin_start, ulp_main_bin_end - ulp_main_bin_start);
    ulp_riscv_run();
    uint32_t d0 = ulp_counter; vTaskDelay(pdMS_TO_TICKS(1000)); uint32_t d1 = ulp_counter;
    printf("idf C ULP program: %lu runs in 1 s, %u bytes\n", (unsigned long)(d1 - d0), (unsigned)(ulp_main_bin_end - ulp_main_bin_start));
    ulp_riscv_halt();
    printf("idf_ulp done\n");
    for (;;) vTaskDelay(1000);
}
