/* ESP32-S3 bring-up: watchdogs, core 1, delays, faults. Clocks: clk.c.
 * No ROM calls: everything is direct register access. */
#include "cache.h"
#include "sys.h"
#include "system.h"
#include "soc.h"
#include "clk.h"
#include "xtensa.h"
#include "console.h"
#include "unwind.h"

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
#if NDF_STACK_GUARD
    { extern void stack_guard_core_off(int); stack_guard_core_off(1); }   /* the ROM start stub runs outside core 1's stack */
#endif
    cpu1_fn = fn;
    REG(SYSTEM_CORE1_CTRL1) = (uint32_t)_start1;         /* ROM core1 stub jumps here */
    REG_SET(SYSTEM_CORE1_CTRL0, BIT(1));                 /* clock on  */
    REG_CLR(SYSTEM_CORE1_CTRL0, BIT(0));                 /* un-stall  */
    REG_SET(SYSTEM_CORE1_CTRL0, BIT(2));                 /* reset pulse */
    REG_CLR(SYSTEM_CORE1_CTRL0, BIT(2));
}

#if NDF_STACK_GUARD
#include "debug.h"
extern char _stack0_bot[], _stack1_bot[], _stack0_top[], _stack1_top[];
static void guard_boot(int core)
{
    uint32_t bot = core ? (uint32_t)_stack1_bot : (uint32_t)_stack0_bot, top = core ? (uint32_t)_stack1_top : (uint32_t)_stack0_top;
    stack_guard(bot + NDF_STACK_GUARD, top, 0);
}
#endif

bool sys_cpu1_running(void) { return cpu1_fn != 0; }

void sys_start1(void)
{
#if NDF_STACK_GUARD
    guard_boot(1);
#endif
#if NDF_MEMPROT
    { extern void memprot_core1(void); memprot_core1(); }
#endif
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

static uint64_t boot_t0, boot_t1;             /* system timer (16 MHz, counts from chip reset) at entry / at main */
void sys_boot_times(uint32_t *rom_us, uint32_t *init_us)
{
    if (rom_us) *rom_us = (uint32_t)(boot_t0 / 16);
    if (init_us) *init_us = (uint32_t)((boot_t1 - boot_t0) / 16);
}

void sys_start0(void)
{
    uint64_t t0 = time_ticks();                 /* first thing: ROM boot + image load ends here */
    for (uint32_t *p = _bss_start; p < _bss_end; p++) *p = 0;
    if (((REG(0x60008038u) & 0x3F) != 5)) {      /* not a deep-sleep wake: RTC_BSS_ATTR zeroed (the loader skipped
                                                     .rtc.data likewise) */
        extern uint32_t _rtc_bss_start[], _rtc_bss_end[];
        for (uint32_t *p = _rtc_bss_start; p < _rtc_bss_end; p++) *p = 0;
    }
    boot_t0 = t0;
    extern void sys_latch_reset_cause(void);
    sys_latch_reset_cause();                      /* before anything resets again (system.c) */
#if NDF_BROWNOUT                                  /* as IDF's non-interrupt mode: the hardware resets the chip (cause 15); no
                                                    handler code in the image. The register resets with the system: every boot */
    { extern void bod_reset_mode(int); bod_reset_mode(NDF_BROWNOUT); }
#endif
#if NDF_GLITCH_RESET                              /* as IDF bootloader_ana_reset_config */
    { extern void glitch_reset(bool); glitch_reset(true); }
    REG(RTC_SWD_WPROTECT) = SWD_WKEY; REG_CLR(0x60008148u, BIT(2)); REG_CLR(RTC_SWD_CONF, BIT(17)); REG(RTC_SWD_WPROTECT) = 0;
#endif
#if NDF_BOOT_ROM_LOG_OFF
    REG_SET(RTC_BASE + 0xC0, BIT(0) | BIT(16));   /* RTC_CNTL_STORE4: ROM skips its boot log on the next reset (no eFuse; power-on clears it) */
#endif
#if NDF_BOOT_WDT_OFF
    wdt_off();
#endif
#if NDF_BOOT_RTC_RESET
    rtc_domain_reset();
#endif
    if (!NDF_BOOT_LOAD_ADDR || clk_cpu_sync() != NDF_CPU_MHZ * 1000000u) {   /* the fast-boot loader already did it */
        clk_bbpll_init();
        clk_cpu_set_mhz(NDF_CPU_MHZ);
    }
#if NDF_XIP
    cache_xip_init();                       /* before any code in flash runs */
#endif
    periph_enable(PERIPH_SYSTIMER);
    console_init();
#if NDF_STACK_GUARD
    guard_boot(0);
#endif
#if NDF_MEMPROT
    { extern int memprot_init(bool); memprot_init(NDF_MEMPROT == 2); }
#endif
    irq_enable_all();                       /* individual sources stay off until attached */
#if NDF_BOOT_BANNER
    sys_print_info();
#endif
    boot_t1 = time_ticks();
    {   /* global constructors (C++ statics, __attribute__((constructor))), in priority order */
        extern void (*__init_array_start[])(void), (*__init_array_end[])(void);
        for (void (**f)(void) = __init_array_start; f < __init_array_end; f++) (*f)();
    }
    main();
    for (;;) {}
}

extern char _text_end[], _stack0_top[], _stack1_top[];
#define TEXT_LO 0x40378000u
#define I2D(a)  ((const volatile uint8_t *)((a) >= 0x42000000u ? (a) - 0x06000000u   /* flash code via the data bus */ \
                                                                 : (a) - 0x6F0000))     /* IRAM code via the DRAM alias */

static bool is_ret_addr(uint32_t w)                 /* w follows a call0 / callx0 in our code */
{
    if (!cache_is_code_addr(w - 3)) return false;
    const volatile uint8_t *i = I2D(w - 3);
    return (i[0] & 0x3F) == 0x05 || (i[0] == 0xC0 && !(i[1] & 0xF0) && !i[2]);
}

static void emit_pc(uint32_t pc) { printf(" 0x%08lx", (unsigned long)pc); }

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
    printf("    Backtrace:");
    bool ok;
    if (frame) ok = ndf_unwind(pc, sp, frame[0], frame[19], emit_pc);   /* from the faulting instruction */
    else {                                                              /* from here: through the caller(s) */
        uint32_t here_sp, here_a15;
        __asm__ volatile("mov %0, a1\n\tmov %1, a15" : "=r"(here_sp), "=r"(here_a15));
        ok = ndf_unwind((uint32_t)&&here, here_sp, 0, here_a15, emit_pc);
    here:;
    }
    if (!ok) {                       /* no unwind table in this image: stack scan (may list stale entries) */
        uint32_t top = core ? (uint32_t)_stack1_top : (uint32_t)_stack0_top;
        if (sp >= top || top - sp > 0x20000) top = sp + 1024;
        emit_pc(pc);
        if (frame && is_ret_addr(frame[0])) emit_pc(frame[0]);
        int n = 0;
        for (uint32_t a = sp & ~3u; a < top && n < 24; a += 4) {
            uint32_t w = *(const volatile uint32_t *)a;
            if (is_ret_addr(w)) { emit_pc(w); n++; }
        }
        printf(" (scan)");
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
    char why[128];
    const char *cs = cause < sizeof c / sizeof c[0] ? c[cause] : (cause >= 32 && cause < 40 ? "coprocessor disabled (vector/PIE in an ISR?)" : "?");
    snprintf(why, sizeof why, "%s, EXCCAUSE %lu (%s), EXCVADDR 0x%08lx%s", k[kind & 3], (unsigned long)cause, cs, (unsigned long)vaddr,
             NDF_MEMPROT && pc >= (((uint32_t)_text_end + 255) & ~255u) && pc < 0x403E0000u ? " - PC in data: fetch blocked by memprot" : "");
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
