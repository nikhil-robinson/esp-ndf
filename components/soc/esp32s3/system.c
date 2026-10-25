/* System information and control for the ESP32-S3. */
#include "system.h"
#include "efuse.h"
#include "flash.h"
#include "clk.h"
#include "console.h"

#define RESET_STATE  0x60008038       /* RTC_CNTL: b0..5 core 0 cause, b6..11 core 1 */
#define OPTIONS0     0x60008000       /* RTC_CNTL: b31 SW_SYS_RST */

/* A reset the warm-boot stage handed to the ROM (esptool, USB) arrives here as a software reset (3); the stage kept
 * the real cause in RTC memory (loader.c CAUSE_NOTE). */
static int cause[2] = { -1, -1 };
void sys_latch_reset_cause(void)               /* at startup (sys.c): read once, consume the note */
{
    uint32_t n = REG(0x50001FECu);
    for (int k = 0; k < 2; k++) {
        int c = (REG(RESET_STATE) >> (k ? 6 : 0)) & 0x3F;
        cause[k] = c == 3 && (n & 0xFFFF0000u) == 0x52430000u ? (int)(n & 0x3F) : c;
    }
    REG(0x50001FECu) = 0;
}
int sys_reset_cause(int core) { if (cause[0] < 0) sys_latch_reset_cause(); return cause[core ? 1 : 0]; }

const char *sys_reset_cause_str(int c)
{
    switch (c) {
    case RESET_POWERON: return "power-on";               case RESET_SW_SYS: return "software (system)";
    case RESET_DEEPSLEEP: return "deep sleep";           case RESET_TG0WDT_SYS: return "TG0 watchdog (system)";
    case RESET_TG1WDT_SYS: return "TG1 watchdog (system)"; case RESET_RTCWDT_SYS: return "RTC watchdog (system)";
    case RESET_TG0WDT_CPU: return "TG0 watchdog (CPU)";  case RESET_SW_CPU: return "software (CPU)";
    case RESET_RTCWDT_CPU: return "RTC watchdog (CPU)";  case RESET_BROWNOUT: return "brownout";
    case RESET_RTCWDT_RTC: return "RTC watchdog (RTC)";  case RESET_TG1WDT_CPU: return "TG1 watchdog (CPU)";
    case RESET_SUPER_WDT: return "super watchdog";       case RESET_CLK_GLITCH: return "clock glitch";
    case RESET_EFUSE: return "eFuse CRC";                case RESET_USB_UART: return "USB-UART (download tool)";
    case RESET_USB_JTAG: return "USB-JTAG (download tool)"; case RESET_POWER_GLITCH: return "power glitch";
    default: return "unknown";
    }
}

void sys_info(sys_info_t *i)
{
    i->model = "ESP32-S3";
    i->rev_major = efuse_bits(1, 184, 2);
    i->rev_minor = efuse_bits(1, 183, 1) << 3 | efuse_bits(1, 114, 3);
    i->package = efuse_bits(1, 117, 3);
    i->cores = SOC_CPU_CORES;
    efuse_mac(i->mac);
    i->flash_id = flash_jedec_id();
    i->flash_size = flash_size();
    i->flash_cached = flash_cached();             /* also picks the read path */
    i->flash_lines = flash_read_lines();
    i->cpu_hz = clk_cpu_hz();
    i->apb_hz = clk_apb_hz();
    i->reset_cause[0] = sys_reset_cause(0);
    i->reset_cause[1] = sys_reset_cause(1);
}

void sys_print_info(void)
{
    sys_info_t i;
    sys_info(&i);
    printf("\nesp-ndf | %s rev v%d.%d pkg %d, %d cores, MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
           i.model, i.rev_major, i.rev_minor, i.package, i.cores, i.mac[0], i.mac[1], i.mac[2], i.mac[3], i.mac[4], i.mac[5]);
    printf("        CPU %lu MHz, APB %lu MHz | flash %06lx %lu MB, %s, %s\n", i.cpu_hz / 1000000, i.apb_hz / 1000000,
           i.flash_id, i.flash_size >> 20, i.flash_lines == 4 ? "quad" : "dual", i.flash_cached ? "cached reads" : "SPI1 reads");
    printf("        reset: core0 %s, core1 %s\n", sys_reset_cause_str(i.reset_cause[0]), sys_reset_cause_str(i.reset_cause[1]));
}

NORETURN void sys_restart(void)
{
    REG_SET(OPTIONS0, BIT(31));
    for (;;) ;
}
