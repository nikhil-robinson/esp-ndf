/* System information and control for the ESP32-P4 (same API as soc/esp32s3/system.c). */
#include "system.h"
#include "clk.h"
#include "efuse.h"
#include "flash.h"
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

void sys_info(sys_info_t *i)
{
    *i = (sys_info_t){ .model = "ESP32-P4", .cores = SOC_CPU_CORES };
    i->rev_major = (uint8_t)(efuse_get("WAFER_VERSION_MAJOR_HI") << 2 | efuse_get("WAFER_VERSION_MAJOR_LO"));
    i->rev_minor = (uint8_t)efuse_get("WAFER_VERSION_MINOR");
    i->package = (uint8_t)efuse_get("PKG_VERSION");
    efuse_mac(i->mac);
    i->flash_id = flash_jedec_id();
    i->flash_size = flash_size();
    i->flash_cached = flash_cached();
    i->flash_lines = (uint8_t)flash_read_lines();
    i->cpu_hz = clk_cpu_hz();
    i->apb_hz = clk_apb_hz();
    i->reset_cause[0] = (uint8_t)sys_reset_cause(0);
    i->reset_cause[1] = (uint8_t)sys_reset_cause(1);
}

void sys_print_info(void)
{
    sys_info_t i;
    sys_info(&i);
    printf("\nesp-ndf | %s rev v%d.%d pkg %d, %d cores, MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
           i.model, i.rev_major, i.rev_minor, i.package, i.cores, i.mac[0], i.mac[1], i.mac[2], i.mac[3], i.mac[4], i.mac[5]);
    printf("        CPU %lu MHz, APB %lu MHz | flash %06lx %lu MB, %s\n", i.cpu_hz / 1000000, i.apb_hz / 1000000,
           i.flash_id, i.flash_size >> 20, i.flash_cached ? "cached reads" : "SPI1 reads");
    printf("        reset: core0 %s, core1 %s\n", sys_reset_cause_str(i.reset_cause[0]), sys_reset_cause_str(i.reset_cause[1]));
}

NORETURN void sys_restart(void)
{
    REG_SET(LP_SYS_CTRL, BIT(1));
    for (;;) ;
}
