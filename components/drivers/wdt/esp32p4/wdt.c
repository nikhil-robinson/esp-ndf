/* P4 watchdogs, same API as drivers/wdt/wdt.c. TIMG0/1 MWDT (same registers as the S3) on XTAL 40 MHz / 40000 = 1 ms
 * ticks (clock source in HP_SYS_CLKRST PERI_CLK_CTRL20/21); LP WDT (ESP-IDF v6.1 lp_wdt_reg.h, lpwdt_ll.h) on RC_SLOW;
 * super WDT. All write-protected: unlock, write, lock. */
#include "wdt.h"
#include "soc.h"
#include "clk.h"
#include "irq.h"
#include "sys.h"
#include "cpu.h"

#define CONFIG0(b)    ((b) + 0x48)  /* b31 en b30:29 stg0 b28:27 stg1 b20:18 cpu rst len b17:15 sys rst len */
#define CONFIG1(b)    ((b) + 0x4C)  /* b31:16 prescaler */
#define CONFIG2(b)    ((b) + 0x50)
#define CONFIG3(b)    ((b) + 0x54)
#define FEED(b)       ((b) + 0x60)
#define WPROTECT(b)   ((b) + 0x64)
#define INT_ENA(b)    ((b) + 0x70)
#define INT_ST(b)     ((b) + 0x78)
#define INT_CLR(b)    ((b) + 0x7C)
#define PRESCALE      40000u
#define STG_INT       1u
#define STG_SYS_RESET 3u
#define STG_RTC_RESET 4u
#define RC_SLOW_HZ    136000u                       /* nominal (IDF SOC_CLK_RC_SLOW_FREQ_APPROX), not calibrated */
#define EFUSE_REPEAT1 (LPPERIPH + 0xD000 + 0x34)    /* b17 WDT_DELAY_SEL */

static const uint8_t src[2] = { SRC_TG0_WDT, SRC_TG1_WDT };
static int irq_line[2] = { -1, -1 };

static void wdt_clk_on(int n)                       /* XTAL source (0), clock enable */
{
    uint32_t a = HP_CLKRST_BASE + (n ? 0x98 : 0x94), s = n ? 26 : 28;
    REG(a) = (REG(a) & ~(3u << s)) | BIT(s + 2);
}

static void wdt_isr(void *arg)
{
    int n = (int)(uintptr_t)arg;
    if (!(REG(INT_ST(TIMG_BASE(n))) & BIT(2))) return;
    uint32_t pc, sp;
    __asm__ volatile("csrr %0, mepc" : "=r"(pc));  /* where the stalled code was */
    __asm__ volatile("mv %0, sp" : "=r"(sp));
    REG(INT_CLR(TIMG_BASE(n))) = BIT(2);
    ndf_panic_report(n ? "watchdog 1: not fed (reset follows)" : "watchdog: main loop not fed (reset follows)", pc, 0, sp);
    for (;;) {}
}

static int mwdt_start(int n, uint32_t timeout_ms, bool report)
{
    uint32_t b = TIMG_BASE(n);
    uint64_t t = (uint64_t)timeout_ms * (40000000u / PRESCALE) / 1000;
    if (!t || t > 0xFFFFFFFFu) return -1;
    if (report && irq_line[n] < 0 && (irq_line[n] = irq_attach(src[n], 3, wdt_isr, (void *)(uintptr_t)n)) < 0) return -1;
    periph_enable(n ? PERIPH_TIMG1 : PERIPH_TIMG0);
    wdt_clk_on(n);
    uint32_t ps = irq_lock();
    REG(WPROTECT(b)) = WDT_WKEY;
    REG(CONFIG0(b)) = 0;
    REG(CONFIG1(b)) = PRESCALE << 16;
    REG(CONFIG2(b)) = (uint32_t)t;
    REG(CONFIG3(b)) = (uint32_t)t;
    REG(FEED(b)) = 1;
    REG(CONFIG0(b)) = BIT(31) | (report ? STG_INT : STG_SYS_RESET) << 29 | (report ? STG_SYS_RESET : 0) << 27
                    | 7u << 18 | 7u << 15 | BIT(22);                 /* b22 conf_update_en: the config crosses into the WDT clock domain */
    REG(WPROTECT(b)) = 0;
    REG(INT_CLR(b)) = BIT(2);
    if (report) REG(INT_ENA(b)) |= BIT(2); else REG(INT_ENA(b)) &= ~BIT(2);
    irq_unlock(ps);
    return 0;
}
static void mwdt_feed(int n) { uint32_t b = TIMG_BASE(n); REG(WPROTECT(b)) = WDT_WKEY; REG(FEED(b)) = 1; REG(WPROTECT(b)) = 0; }
static void mwdt_stop(int n)
{
    uint32_t b = TIMG_BASE(n), ps = irq_lock();
    REG(WPROTECT(b)) = WDT_WKEY; REG(CONFIG0(b)) = BIT(22); REG(WPROTECT(b)) = 0;
    REG(INT_ENA(b)) &= ~BIT(2);
    irq_unlock(ps);
}

int  wdt_start(uint32_t timeout_ms, bool report) { return mwdt_start(0, timeout_ms, report); }
void wdt_feed(void) { mwdt_feed(0); }
void wdt_stop(void) { mwdt_stop(0); }
int  wdt1_start(uint32_t timeout_ms, bool report) { return mwdt_start(1, timeout_ms, report); }
void wdt1_feed(void) { mwdt_feed(1); }
void wdt1_stop(void) { mwdt_stop(1); }

/* ---- LP (RTC) watchdog: CONFIG0 b31 en, b30:28 stg0, b18:16 cpu reset len, b15:13 sys reset len, b11/b10 core0/1
 * reset en; CONFIG1 stage 0 ticks >> (1 + eFuse WDT_DELAY_SEL) (lpwdt_ll_config_stage) ---- */
int rtc_wdt_start(uint32_t timeout_ms, bool chip_reset)
{
    uint32_t shift = 1 + ((REG(EFUSE_REPEAT1) >> 17) & 1);
    uint64_t t = ((uint64_t)timeout_ms * RC_SLOW_HZ / 1000) >> shift;
    if (!t || t > 0xFFFFFFFFu) return -1;
    uint32_t ps = irq_lock();
    REG(LP_WDT_WPROTECT) = WDT_WKEY;
    REG(LP_WDT_CONFIG0) = 0;
    REG(LP_WDT_CONFIG0 + 4) = (uint32_t)t;
    REG(LP_WDT_FEED) = BIT(31);
    REG(LP_WDT_CONFIG0) = BIT(31) | (chip_reset ? STG_RTC_RESET : STG_SYS_RESET) << 28 | 7u << 16 | 7u << 13 | BIT(11) | BIT(10);
    REG(LP_WDT_WPROTECT) = 0;
    irq_unlock(ps);
    return 0;
}
void rtc_wdt_feed(void) { REG(LP_WDT_WPROTECT) = WDT_WKEY; REG(LP_WDT_FEED) = BIT(31); REG(LP_WDT_WPROTECT) = 0; }
void rtc_wdt_stop(void) { REG(LP_WDT_WPROTECT) = WDT_WKEY; REG(LP_WDT_CONFIG0) = 0; REG(LP_WDT_WPROTECT) = 0; }

/* ---- super watchdog: SWD_CONFIG b31 feed, b30 disable, b18 auto feed ---- */
void swd_enable(bool on)
{
    REG(LP_WDT_SWD_WPROTECT) = SWD_WKEY;
    uint32_t c = REG(LP_WDT_SWD_CONFIG) & ~BIT(31);
    REG(LP_WDT_SWD_CONFIG) = on ? (c & ~(BIT(30) | BIT(18))) | BIT(31) : c | BIT(30);
    REG(LP_WDT_SWD_WPROTECT) = 0;
}
void swd_feed(void)
{
    REG(LP_WDT_SWD_WPROTECT) = SWD_WKEY; REG_SET(LP_WDT_SWD_CONFIG, BIT(31)); REG(LP_WDT_SWD_WPROTECT) = 0;
}
