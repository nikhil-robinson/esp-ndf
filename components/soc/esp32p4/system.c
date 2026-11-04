/* System information and control for the ESP32-P4 (same API as soc/esp32s3/system.c). */
#include "system.h"
#include "clk.h"
#include "console.h"
#include "soc.h"

#define LP_SYS_CTRL  (LP_SYS_BASE + 0x08)       /* b1 SYS_SW_RST */

static int cause[2] = { -1, -1 };
void sys_latch_reset_cause(void)
{
    uint32_t r = REG(LP_CLKRST_RESET_CAUSE);
    cause[0] = (r >> 7) & 0x3F;
    cause[1] = (r >> 14) & 0x3F;
}
int sys_reset_cause(int core) { if (cause[0] < 0) sys_latch_reset_cause(); return cause[core ? 1 : 0]; }

/* P4 codes (IDF soc/esp32p4/include/soc/reset_reasons.h); the common ones equal the S3's RESET_* values */
const char *sys_reset_cause_str(int c)
{
    switch (c) {
    case 0x01: return "power-on";                 case 0x03: return "software (system)";
    case 0x05: return "deep sleep / PMU power down"; case 0x07: return "MWDT (system)";
    case 0x09: return "RTC watchdog (system)";    case 0x0B: return "MWDT (CPU)";
    case 0x0C: return "software (CPU)";           case 0x0D: return "RTC watchdog (CPU)";
    case 0x0F: return "brownout";                 case 0x10: return "RTC watchdog (RTC)";
    case 0x12: return "super watchdog";           case 0x13: return "power glitch";
    case 0x14: return "eFuse CRC";                case 0x16: return "USB-JTAG (download tool)";
    case 0x17: return "USB-UART (download tool)"; case 0x18: return "JTAG";
    case 0x1A: return "CPU lockup";               case 0x1B: return "power supply glitch";
    default: return "unknown";
    }
}

/* ponytail: eFuse (revision, MAC) and flash fields stay 0 until the P4 eFuse and flash drivers exist */
void sys_info(sys_info_t *i)
{
    *i = (sys_info_t){ .model = "ESP32-P4", .cores = SOC_CPU_CORES };
    i->cpu_hz = clk_cpu_hz();
    i->apb_hz = clk_apb_hz();
    i->reset_cause[0] = (uint8_t)sys_reset_cause(0);
    i->reset_cause[1] = (uint8_t)sys_reset_cause(1);
}

void sys_print_info(void)
{
    sys_info_t i;
    sys_info(&i);
    printf("\nesp-ndf | %s, %d cores | CPU %lu MHz, APB %lu MHz\n", i.model, i.cores, i.cpu_hz / 1000000, i.apb_hz / 1000000);
    printf("        reset: core0 %s, core1 %s\n", sys_reset_cause_str(i.reset_cause[0]), sys_reset_cause_str(i.reset_cause[1]));
}

NORETURN void sys_restart(void)
{
    REG_SET(LP_SYS_CTRL, BIT(1));
    for (;;) ;
}
