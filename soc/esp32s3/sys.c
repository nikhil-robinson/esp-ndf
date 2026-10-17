/* ESP32-S3 bring-up: watchdogs, core 1, delays, faults. Clocks: clk.c.
 * No ROM calls: everything is direct register access. */
#include "sys.h"
#include "soc.h"
#include "clk.h"
#include "xtensa.h"
#include "console.h"

extern uint32_t _bss_start[], _bss_end[];
extern void _start1(void);
int main(void);
void clk_bbpll_init(void);

static void (*volatile cpu1_fn)(void);

uint32_t sys_cpu_hz(void) { return clk_cpu_hz(); }

void delay_us(uint32_t us)
{
    uint32_t t = ccount(), n = us * (clk_cpu_hz() / 1000000);
    while (ccount() - t < n) {}
}

uint32_t systimer_lo(void)
{
    REG(SYSTIMER_UNIT0_OP) = BIT(30);
    while (!(REG(SYSTIMER_UNIT0_OP) & BIT(29))) {}
    return REG(SYSTIMER_UNIT0_LO);
}

static void wdt_off(void)
{
    for (int n = 0; n < 2; n++) {
        REG(TIMG_WDTWPROTECT(n)) = WDT_WKEY; REG(TIMG_WDTCONFIG0(n)) = 0; REG(TIMG_WDTWPROTECT(n)) = 0;
    }
    REG(RTC_WDTWPROTECT) = WDT_WKEY; REG(RTC_WDTCONFIG0) = 0; REG(RTC_WDTWPROTECT) = 0;
    REG(RTC_SWD_WPROTECT) = SWD_WKEY; REG_SET(RTC_SWD_CONF, BIT(31)); REG(RTC_SWD_WPROTECT) = 0;
}

void cpu1_start(void (*fn)(void))
{
    cpu1_fn = fn;
    REG(SYSTEM_CORE1_CTRL1) = (uint32_t)_start1;         /* ROM core1 stub jumps here */
    REG_SET(SYSTEM_CORE1_CTRL0, BIT(1));                 /* clock on  */
    REG_CLR(SYSTEM_CORE1_CTRL0, BIT(0));                 /* un-stall  */
    REG_SET(SYSTEM_CORE1_CTRL0, BIT(2));                 /* reset pulse */
    REG_CLR(SYSTEM_CORE1_CTRL0, BIT(2));
}

void sys_start1(void)
{
    cpu1_fn();
    for (;;) {}
}

void sys_start0(void)
{
    for (uint32_t *p = _bss_start; p < _bss_end; p++) *p = 0;
    wdt_off();
    clk_bbpll_init();
    clk_cpu_set_mhz(240);
    periph_enable(PERIPH_SYSTIMER);
    console_init();
    main();
    for (;;) {}
}

NORETURN void ndf_fault(uint32_t kind, uint32_t cause, uint32_t pc, uint32_t vaddr)
{
    static const char *k[] = { "?", "exception", "double exception", "unexpected vector" };
    printf("\n*** FAULT core%d: %s, EXCCAUSE=%lu PC=0x%08lx EXCVADDR=0x%08lx\n",
           core_id(), k[kind & 3], cause, pc, vaddr);
    for (;;) {}
}
