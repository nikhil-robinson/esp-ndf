/* ESP32-S3 bring-up: watchdogs, core 1, delays, faults. Clocks: clk.c.
 * No ROM calls: everything is direct register access. */
#include "sys.h"
#include "system.h"
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
    uint32_t mhz = clk_cpu_hz() / 1000000;
    for (; us > 1000000; us -= 1000000) {          /* keep us * mhz inside 32 bits */
        uint32_t t = ccount();
        while (ccount() - t < 1000000 * mhz) {}
    }
    uint32_t t = ccount(), n = us * mhz;
    while (ccount() - t < n) {}
}

void delay_ms(uint32_t ms)
{
    for (; ms > 1000; ms -= 1000) delay_us(1000000);
    delay_us(ms * 1000);
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
    irq_enable_all();                       /* individual sources stay off until attached */
    cpu1_fn();
    for (;;) {}
}

/* RTC-domain state survives chip resets: return pads and the touch scanner
 * to their power-on state so every boot starts identical. */
static void rtc_domain_reset(void)
{
    uint32_t held = REG(0x600080D8);                                      /* RTC pad hold survives reset */
    for (int n = 0; n <= 21; n++)
        if (n != 19 && n != 20 && !(held >> n & 1))
            REG_CLR(0x60008484 + 4 * n, BIT(19));                         /* digital mux (not USB, not held) */
    REG_CLR(0x6000810C, BIT(13));                                         /* touch scan timer off */
    REG_FIELD(0x60008110, 10, 15, 0);                                     /* touch pad map empty */
}

void sys_start0(void)
{
    for (uint32_t *p = _bss_start; p < _bss_end; p++) *p = 0;
#if NDF_BOOT_WDT_OFF
    wdt_off();
#endif
#if NDF_BOOT_RTC_RESET
    rtc_domain_reset();
#endif
    clk_bbpll_init();
    clk_cpu_set_mhz(NDF_CPU_MHZ);
    periph_enable(PERIPH_SYSTIMER);
    console_init();
    irq_enable_all();                       /* individual sources stay off until attached */
#if NDF_BOOT_BANNER
    sys_print_info();
#endif
    main();
    for (;;) {}
}

extern char _text_end[], _stack0_top[], _stack1_top[];
#define TEXT_LO 0x40378000u
#define I2D(a)  ((const volatile uint8_t *)((a) - 0x6F0000))  /* IRAM code read through the DRAM alias */

static bool is_ret_addr(uint32_t w)                 /* w follows a call0 / callx0 in our code */
{
    if (w < TEXT_LO + 3 || w >= (uint32_t)_text_end) return false;
    const volatile uint8_t *i = I2D(w - 3);
    return (i[0] & 0x3F) == 0x05 || (i[0] == 0xC0 && !(i[1] & 0xF0) && !i[2]);
}

void ndf_panic_report(const char *why, uint32_t pc, const uint32_t *frame, uint32_t sp)
{
    irq_lock();
    int core = core_id();
    printf("\n*** PANIC core%d: %s\n    PC 0x%08lx\n", core, why, (unsigned long)pc);
    if (frame) {
        static const uint8_t ix[16] = { 0, 0xFF, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 16, 17, 18, 19 };   /* a1 = sp */
        for (int r = 0; r < 16; r++) {
            uint32_t v = r == 1 ? sp : frame[ix[r]];
            printf("    a%-2d 0x%08lx%s", r, (unsigned long)v, (r & 3) == 3 ? "\n" : "");
        }
        printf("    SAR 0x%08lx LBEG 0x%08lx LEND 0x%08lx LCOUNT 0x%08lx\n",
               (unsigned long)frame[11], (unsigned long)frame[12], (unsigned long)frame[13], (unsigned long)frame[14]);
    }
    uint32_t top = core ? (uint32_t)_stack1_top : (uint32_t)_stack0_top;
    if (sp >= top || top - sp > 0x20000) top = sp + 1024;                   /* sp outside our stacks: scan 1 KB */
    printf("    Backtrace: 0x%08lx", (unsigned long)pc);
    if (frame && is_ret_addr(frame[0])) printf(" 0x%08lx", (unsigned long)frame[0]);
    int n = 0;
    for (uint32_t a = sp & ~3u; a < top && n < 24; a += 4) {
        uint32_t w = *(const volatile uint32_t *)a;
        if (is_ret_addr(w)) { printf(" 0x%08lx", (unsigned long)w); n++; }
    }
    printf("\n");
}

NORETURN void ndf_panic(const char *why, uint32_t pc, const uint32_t *frame, uint32_t sp)
{
    ndf_panic_report(why, pc, frame, sp);
#if NDF_PANIC_RESTART
    delay_us(20000);                                      /* let the console drain */
    sys_restart();
#else
    for (;;) {}
#endif
}

NORETURN void ndf_fault(uint32_t kind, uint32_t cause, uint32_t pc, uint32_t vaddr, uint32_t *frame, uint32_t sp)
{
    static const char *k[] = { "?", "exception", "double exception", "unexpected vector" };
    static const char *c[] = { "illegal instruction", "syscall", "instruction fetch error", "load/store error",
                               "level-1 interrupt", "alloca", "integer divide by zero", "?", "privileged",
                               "load/store alignment", "?", "?", "instr PIF data error", "load/store PIF data error",
                               "instr PIF address error", "load/store PIF address error", "inst TLB miss", "inst TLB multi hit",
                               "inst fetch privilege", "?", "inst fetch prohibited", "?", "?", "?", "load/store TLB miss",
                               "load/store TLB multi hit", "load/store privilege", "?", "load prohibited", "store prohibited" };
    char why[96];
    const char *cs = cause < sizeof c / sizeof c[0] ? c[cause] : (cause >= 32 && cause < 40 ? "coprocessor disabled (float/vector in an ISR?)" : "?");
    snprintf(why, sizeof why, "%s, EXCCAUSE %lu (%s), EXCVADDR 0x%08lx", k[kind & 3], (unsigned long)cause, cs, (unsigned long)vaddr);
    ndf_panic(why, pc, frame, sp);
}

NORETURN void ndf_assert_fail(const char *file, int line, const char *cond)
{
    char why[128];
    uint32_t sp;
    __asm__ volatile("mov %0, a1" : "=r"(sp));
    snprintf(why, sizeof why, "assert %s:%d: %s", file, line, cond);
    ndf_panic(why, (uint32_t)__builtin_return_address(0) - 3, 0, sp);
}
