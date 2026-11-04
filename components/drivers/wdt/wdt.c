/* Watchdogs (TRM ch. 13, 14; ESP-IDF hal mwdt_ll.h / rwdt_ll.h). TIMG0/TIMG1 MWDT: 32-bit stage counters on
 * APB / 40000 (0.5 ms at 80 MHz); stage 0 = interrupt (report) or system reset, stage 1 = system reset. RTC WDT: RTC
 * slow clock, stage 0 (multiplied by 2 << eFuse WDT_DELAY_SEL, as rwdt_ll_config_stage notes) = system or chip
 * (RTC) reset. Super WDT: fixed timeout, needs feeding once auto-feed is off. All write-protected: unlock, write, lock. */
#include "wdt.h"
#include "soc.h"
#include "clk.h"
#include "irq.h"
#include "sys.h"
#include "cpu.h"
#include "rtcclk.h"

#define CONFIG0(b)    ((b) + 0x48)  /* b31 en b30:29 stg0 b28:27 stg1 b20:18 cpu rst len b17:15 sys rst len */
#define CONFIG1(b)    ((b) + 0x4C)  /* b31:16 prescaler */
#define CONFIG2(b)    ((b) + 0x50)  /* stage 0 ticks */
#define CONFIG3(b)    ((b) + 0x54)  /* stage 1 ticks */
#define FEED(b)       ((b) + 0x60)
#define WPROTECT(b)   ((b) + 0x64)
#define INT_ENA(b)    ((b) + 0x70)  /* b2 wdt (b0/b1 are the general timers: read-modify-write) */
#define INT_ST(b)     ((b) + 0x78)  /* b2 wdt */
#define INT_CLR(b)    ((b) + 0x7C)
#define PRESCALE      40000u
#define STG_INT       1u
#define STG_SYS_RESET 3u
#define STG_RTC_RESET 4u

static const uint32_t base[2] = { 0x6001F000, 0x60020000 };
static const uint8_t src[2] = { 52, 55 };                  /* TG0 / TG1 WDT interrupt sources */
static int irq_line[2] = { -1, -1 };

static void wdt_isr(void *arg)
{
    int n = (int)(uintptr_t)arg;
    if (!(REG(INT_ST(base[n])) & BIT(2))) return;     /* the line is shared: only our own interrupt is a watchdog */
    uint32_t pc, sp;
    __asm__ volatile("rsr.epc3 %0" : "=r"(pc));   /* where the stalled code was */
    __asm__ volatile("mov %0, a1" : "=r"(sp));
    REG(INT_CLR(base[n])) = BIT(2);
    ndf_panic_report(n ? "watchdog 1: not fed (reset follows)" : "watchdog: main loop not fed (reset follows)", pc, 0, sp);
    for (;;) {}                                   /* stage 1 resets the chip: cause reads TG0/TG1 watchdog */
}

static int mwdt_start(int n, uint32_t timeout_ms, bool report)
{
    uint32_t b = base[n];
    uint64_t t = (uint64_t)timeout_ms * (clk_apb_hz() / PRESCALE) / 1000;
    if (!t || t > 0xFFFFFFFFu) return -1;
    if (report && irq_line[n] < 0 && (irq_line[n] = irq_attach(src[n], 3, wdt_isr, (void *)(uintptr_t)n)) < 0) return -1;
    uint32_t ps = irq_lock();
    REG(WPROTECT(b)) = WDT_WKEY;
    REG(CONFIG0(b)) = 0;
    REG(CONFIG1(b)) = PRESCALE << 16;
    REG(CONFIG2(b)) = (uint32_t)t;
    REG(CONFIG3(b)) = (uint32_t)t;
    REG(FEED(b)) = 1;
    REG(CONFIG0(b)) = BIT(31) | (report ? STG_INT : STG_SYS_RESET) << 29 | (report ? STG_SYS_RESET : 0) << 27
                    | 7u << 18 | 7u << 15;                           /* longest reset pulses (3.2 us) */
    REG(WPROTECT(b)) = 0;
    REG(INT_CLR(b)) = BIT(2);
    if (report) REG(INT_ENA(b)) |= BIT(2); else REG(INT_ENA(b)) &= ~BIT(2);
    irq_unlock(ps);
    return 0;
}
static void mwdt_feed(int n) { uint32_t b = base[n]; REG(WPROTECT(b)) = WDT_WKEY; REG(FEED(b)) = 1; REG(WPROTECT(b)) = 0; }
static void mwdt_stop(int n)
{
    uint32_t b = base[n], ps = irq_lock();
    REG(WPROTECT(b)) = WDT_WKEY; REG(CONFIG0(b)) = 0; REG(WPROTECT(b)) = 0;
    REG(INT_ENA(b)) &= ~BIT(2);
    irq_unlock(ps);
}

int  wdt_start(uint32_t timeout_ms, bool report) { return mwdt_start(0, timeout_ms, report); }
void wdt_feed(void) { mwdt_feed(0); }
void wdt_stop(void) { mwdt_stop(0); }
int  wdt1_start(uint32_t timeout_ms, bool report) { return mwdt_start(1, timeout_ms, report); }
void wdt1_feed(void) { mwdt_feed(1); }
void wdt1_stop(void) { mwdt_stop(1); }

/* ---- RTC watchdog ---- */
#define RWDT_CFG(i)   (0x60008098u + 4 * (i))   /* CONFIG0, stage 0..3 timeouts */
#define RWDT_FEED     0x600080ACu               /* b31 */
#define RWDT_WP       0x600080B0u

int rtc_wdt_start(uint32_t timeout_ms, bool chip_reset)
{
    uint32_t mult = 2u << ((REG(0x60007034u) >> 16) & 3);           /* stage 0 multiplier (eFuse WDT_DELAY_SEL) */
    uint64_t t = (uint64_t)timeout_ms * rtc_slow_hz() / 1000 / mult;
    if (!t || t > 0xFFFFFFFFu) return -1;
    uint32_t ps = irq_lock();
    REG(RWDT_WP) = WDT_WKEY;
    REG(RWDT_CFG(0)) = 0;
    REG(RWDT_CFG(1)) = (uint32_t)t;
    REG(RWDT_FEED) = BIT(31);
    REG(RWDT_CFG(0)) = BIT(31) | (chip_reset ? STG_RTC_RESET : STG_SYS_RESET) << 28 | 7u << 16 | 7u << 13 |
                       BIT(11) | BIT(10) | (chip_reset ? BIT(8) : 0) | 20u;   /* both CPUs, chip-reset pulse width */
    REG(RWDT_WP) = 0;
    irq_unlock(ps);
    return 0;
}
void rtc_wdt_feed(void) { REG(RWDT_WP) = WDT_WKEY; REG(RWDT_FEED) = BIT(31); REG(RWDT_WP) = 0; }
void rtc_wdt_stop(void) { REG(RWDT_WP) = WDT_WKEY; REG(RWDT_CFG(0)) = 0; REG(RWDT_WP) = 0; }

/* ---- super watchdog: auto-fed at boot (sys.c); swd_enable(true) makes swd_feed() mandatory ---- */
static uint32_t swd_conf;                        /* SWD_CONF without the feed bit: feeding is one write, no slow read */
void swd_enable(bool on)
{
    REG(RTC_SWD_WPROTECT) = SWD_WKEY;
    swd_conf = REG(RTC_SWD_CONF) & ~BIT(29);
    if (on) { swd_conf &= ~BIT(31); REG(RTC_SWD_CONF) = swd_conf | BIT(29); }
    else { swd_conf |= BIT(31); REG(RTC_SWD_CONF) = swd_conf; }
    REG(RTC_SWD_WPROTECT) = 0;
}
void swd_feed(void)
{
    if (!swd_conf) swd_conf = REG(RTC_SWD_CONF) & ~BIT(29);
    REG(RTC_SWD_WPROTECT) = SWD_WKEY; REG(RTC_SWD_CONF) = swd_conf | BIT(29); REG(RTC_SWD_WPROTECT) = 0;
}
