/* System information and control: chip, flash, clocks, reset reason, restart. */
#pragma once
#include "ndf.h"

typedef struct {
    const char *model;                  /* "ESP32-S3" */
    uint8_t  rev_major, rev_minor;      /* silicon revision (eFuse wafer version) */
    uint8_t  package;                   /* eFuse PKG_VERSION */
    uint8_t  cores;
    uint8_t  mac[6];                    /* factory base MAC */
    uint32_t flash_id;                  /* JEDEC: manufacturer << 16 | type << 8 | capacity */
    uint32_t flash_size;                /* bytes */
    uint8_t  flash_lines;               /* 4 = quad, 2 = dual */
    bool     flash_cached;              /* reads through the data cache */
    uint32_t cpu_hz, apb_hz;
    uint8_t  reset_cause[2];            /* per core, RESET_* below */
} sys_info_t;

/* reset causes (RTC_CNTL_RESET_STATE, same codes as the ROM) */
enum { RESET_POWERON = 1, RESET_SW_SYS = 3, RESET_DEEPSLEEP = 5, RESET_TG0WDT_SYS = 7, RESET_TG1WDT_SYS = 8,
       RESET_RTCWDT_SYS = 9, RESET_TG0WDT_CPU = 11, RESET_SW_CPU = 12, RESET_RTCWDT_CPU = 13, RESET_BROWNOUT = 15,
       RESET_RTCWDT_RTC = 16, RESET_TG1WDT_CPU = 17, RESET_SUPER_WDT = 18, RESET_CLK_GLITCH = 19, RESET_EFUSE = 20,
       RESET_USB_UART = 21, RESET_USB_JTAG = 22, RESET_POWER_GLITCH = 23 };

void        sys_info(sys_info_t *info);
int         sys_reset_cause(int core);
const char *sys_reset_cause_str(int cause);
void        sys_print_info(void);          /* one-screen summary on the console */
/* Last crash (fault, assert, watchdog report), kept in RTC memory through any reset but power-on; returned once. */
typedef struct { uint32_t magic, pc, uptime_ms, bt[8]; uint8_t core, n_bt; char why[64]; } sys_crash_t;
bool        sys_last_crash(sys_crash_t *c);
uint32_t    sys_crash_count(void);                  /* crashes since power-on */
NORETURN void sys_restart(void);           /* full digital system reset (RTC domain kept) */
/* boot time: chip reset -> our first instruction (ROM boot + image load), and our init until main() */
void        sys_boot_times(uint32_t *rom_us, uint32_t *init_us);
