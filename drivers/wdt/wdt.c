/* TIMG0 MWDT (TRM ch. 13): 32-bit stage counters on APB / 40000 (0.5 ms at
 * 80 MHz). Stage 0 = interrupt (report) or system reset, stage 1 = system
 * reset. Registers are write-protected: unlock, write, lock. */
#include "wdt.h"
#include "soc.h"
#include "clk.h"
#include "irq.h"
#include "sys.h"
#include "xtensa.h"

#define WDT0          0x6001F000
#define CONFIG0       (WDT0 + 0x48)  /* b31 en b30:29 stg0 b28:27 stg1 b20:18 cpu rst len b17:15 sys rst len */
#define CONFIG1       (WDT0 + 0x4C)  /* b31:16 prescaler */
#define CONFIG2       (WDT0 + 0x50)  /* stage 0 ticks */
#define CONFIG3       (WDT0 + 0x54)  /* stage 1 ticks */
#define FEED          (WDT0 + 0x60)
#define WPROTECT      (WDT0 + 0x64)
#define INT_ENA       (WDT0 + 0x70)  /* b2 wdt (b0/b1 are the general timers: read-modify-write) */
#define INT_CLR       (WDT0 + 0x7C)
#define SRC_TG0_WDT   52
#define PRESCALE      40000u
#define STG_INT       1u
#define STG_SYS_RESET 3u

static int irq_line = -1;

static void wdt_isr(void *arg)
{
    (void)arg;
    uint32_t pc, sp;
    __asm__ volatile("rsr.epc3 %0" : "=r"(pc));   /* where the stalled code was */
    __asm__ volatile("mov %0, a1" : "=r"(sp));
    REG(INT_CLR) = BIT(2);
    ndf_panic_report("watchdog: main loop not fed (reset follows)", pc, 0, sp);
    for (;;) {}                                   /* stage 1 resets the chip: cause reads TG0 watchdog */
}

int wdt_start(uint32_t timeout_ms, bool report)
{
    uint64_t t = (uint64_t)timeout_ms * (clk_apb_hz() / PRESCALE) / 1000;
    if (!t || t > 0xFFFFFFFFu) return -1;
    if (report && irq_line < 0 && (irq_line = irq_attach(SRC_TG0_WDT, 3, wdt_isr, 0)) < 0) return -1;
    uint32_t ps = irq_lock();
    REG(WPROTECT) = WDT_WKEY;
    REG(CONFIG0) = 0;
    REG(CONFIG1) = PRESCALE << 16;
    REG(CONFIG2) = (uint32_t)t;
    REG(CONFIG3) = (uint32_t)t;
    REG(FEED) = 1;
    REG(CONFIG0) = BIT(31) | (report ? STG_INT : STG_SYS_RESET) << 29 | (report ? STG_SYS_RESET : 0) << 27
                 | 7u << 18 | 7u << 15;                              /* longest reset pulses (3.2 us) */
    REG(WPROTECT) = 0;
    REG(INT_CLR) = BIT(2);
    if (report) REG(INT_ENA) |= BIT(2); else REG(INT_ENA) &= ~BIT(2);
    irq_unlock(ps);
    return 0;
}

void wdt_feed(void)
{
    REG(WPROTECT) = WDT_WKEY;
    REG(FEED) = 1;
    REG(WPROTECT) = 0;
}

void wdt_stop(void)
{
    uint32_t ps = irq_lock();
    REG(WPROTECT) = WDT_WKEY;
    REG(CONFIG0) = 0;
    REG(WPROTECT) = 0;
    REG(INT_ENA) &= ~BIT(2);
    irq_unlock(ps);
}
