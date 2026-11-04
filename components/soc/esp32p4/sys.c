/* ESP32-P4 bring-up: watchdogs, clocks, core 1, delays, faults. Same API and behaviour as soc/esp32s3/sys.c.
 * No ROM calls: everything is direct register access (sequences from ESP-IDF v6.1, see each function). */
#include "spin.h"
#include "sys.h"
#include "system.h"
#include "soc.h"
#include "clk.h"
#include "cpu.h"
#include "console.h"
#include "unwind.h"
#include "irq.h"
#include "debug.h"

uintptr_t __stack_chk_guard = 0x5A3C9E17u;
NORETURN void __stack_chk_fail(void)
{
    ndf_panic("stack smashing detected (canary)", (uint32_t)(uintptr_t)__builtin_return_address(0), 0, 0);
}
/* ponytail: canary from timer jitter, not the TRNG; read LP_TRNG once its driver exists */
static __attribute__((no_stack_protector)) void canary_init(void)
{
    uint32_t g = 0;
    for (int i = 0; i < 64; i++) { g = (g << 5 | g >> 27) ^ REG(SYSTIMER_UNIT0_LO) ^ ccount(); for (volatile int d = 0; d < 50; d++) {} }
    __stack_chk_guard = g & 0xFFFFFF00u;
}

extern uint32_t _bss_start[], _bss_end[];
extern void _start1(void);
int main(void);

static void (*volatile cpu1_fn)(void);

uint32_t sys_cpu_hz(void) { return clk_cpu_hz(); }

void delay_us(uint32_t us)
{
    uint32_t mhz = clk_cpu_hz() / 1000000;
    for (; us > 1000000; us -= 1000000) {
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
    SPIN_US(REG(SYSTIMER_UNIT0_OP) & BIT(29), 100);
    return REG(SYSTIMER_UNIT0_LO);
}

/* TIMG0/1 MWDT, LP (RTC) WDT off; super WDT disabled (IDF bootloader_init / lpwdt_ll / mwdt_ll) */
static void wdt_off(void)
{
    for (int n = 0; n < 2; n++) {
        REG(TIMG_WDTWPROTECT(n)) = WDT_WKEY; REG(TIMG_WDTCONFIG0(n)) = 0; REG(TIMG_WDTWPROTECT(n)) = 0;
    }
    REG(LP_WDT_WPROTECT) = WDT_WKEY; REG(LP_WDT_CONFIG0) = 0; REG(LP_WDT_WPROTECT) = 0;
    REG(LP_WDT_SWD_WPROTECT) = SWD_WKEY; REG_SET(LP_WDT_SWD_CONFIG, BIT(30)); REG(LP_WDT_SWD_WPROTECT) = 0;
}

/* IDF start_other_core: unstall (PMU stall code 0xFF), clock on, out of reset, boot address (LP_SYS, ROM reads it) */
#if NDF_STACK_GUARD
extern char _stack0_bot[], _stack1_bot[], _stack0_top[], _stack1_top[];
extern void stack_guard_core_off(int);
static void guard_boot(int core)
{
    uint32_t bot = core ? (uint32_t)_stack1_bot : (uint32_t)_stack0_bot, top = core ? (uint32_t)_stack1_top : (uint32_t)_stack0_top;
    stack_guard(bot + NDF_STACK_GUARD, top, 0);
}
#endif

void cpu1_start(void (*fn)(void))
{
#if NDF_STACK_GUARD
    stack_guard_core_off(1);                          /* the ROM start stub runs outside core 1's stack */
#endif
    cpu1_fn = fn;
    REG(LP_SYS_BOOT_ADDR_HP_CORE1) = (uint32_t)_start1;
    REG_FIELD(PMU_CPU_SW_STALL, 16, 8, 0xFF);
    SPIN_US(!(REG(HP_SYS_CORESTALLED_ST) & BIT(1)), 1000);
    REG_SET(HP_CLKRST_SOC_CLK_CTRL0, BIT(4));
    REG_CLR(HP_CLKRST_HP_RST_EN0, BIT(8));
}

bool sys_cpu1_running(void) { return cpu1_fn != 0; }

void sys_start1(void)
{
#if NDF_STACK_GUARD
    guard_boot(1);
#endif
    irq_enable_all();
    cpu1_fn();
    for (;;) {}
}

static uint64_t boot_t0, boot_t1;
void sys_boot_times(uint32_t *rom_us, uint32_t *init_us)
{
    if (rom_us) *rom_us = (uint32_t)(boot_t0 / 16);
    if (init_us) *init_us = (uint32_t)((boot_t1 - boot_t0) / 16);
}

__attribute__((no_stack_protector)) void sys_start0(void)
{
    uint64_t t0 = time_ticks();
    for (uint32_t *p = _bss_start; p < _bss_end; p++) *p = 0;
    canary_init();
    {   /* RTC_BSS_ATTR in LP RAM: zeroed except after a deep-sleep wake (cause 5) */
        extern uint32_t _rtc_bss_start[], _rtc_bss_end[];
        if (((REG(LP_CLKRST_RESET_CAUSE) >> 7) & 0x3F) != 5)
            for (uint32_t *p = _rtc_bss_start; p < _rtc_bss_end; p++) *p = 0;
    }
    boot_t0 = t0;
    extern void sys_latch_reset_cause(void);
    sys_latch_reset_cause();
#if NDF_BOOT_WDT_OFF
    wdt_off();
#endif
    clk_cpu_set_mhz(NDF_CPU_MHZ);
    periph_enable(PERIPH_SYSTIMER);
    console_init();
#if NDF_STACK_GUARD
    guard_boot(0);
#endif
    irq_enable_all();
#if NDF_BOOT_BANNER
    sys_print_info();
#endif
    boot_t1 = time_ticks();
    {
        extern void (*__init_array_start[])(void), (*__init_array_end[])(void);
        for (void (**f)(void) = __init_array_start; f < __init_array_end; f++) (*f)();
    }
    main();
    for (;;) {}
}

/* ---- faults ---------------------------------------------------------------- */
extern char _text_end[], _stack0_top[], _stack1_top[];
#define TEXT_LO 0x4FF80000u

static bool is_ret_addr(uint32_t w)           /* w follows a jal / jalr (4 bytes) or c.jal / c.jalr (2 bytes) */
{
    if (w < TEXT_LO + 4 || w >= (uint32_t)_text_end || (w & 1)) return false;
    const volatile uint16_t *h = (const volatile uint16_t *)(w - 4);
    uint32_t i = h[0] | (uint32_t)h[1] << 16;
    if ((i & 0x7F) == 0x6F && ((i >> 7) & 31) == 1) return true;                 /* jal ra, ... */
    if ((i & 0x707F) == 0x67 && ((i >> 7) & 31) == 1) return true;               /* jalr ra, ... */
    uint16_t c = h[1];
    return (c & 0xF07F) == 0x9002 && (c & 0x0F80);                                /* c.jalr rs1 */
}

static RTC_NOINIT_ATTR sys_crash_t crash_rec;
static RTC_NOINIT_ATTR uint32_t crash_count_v;
#define CRASH_MAGIC 0x43525348u

static void emit_pc(uint32_t pc)
{
    printf(" 0x%08lx", (unsigned long)pc);
    if (crash_rec.n_bt < 8) crash_rec.bt[crash_rec.n_bt++] = pc;
}

bool sys_last_crash(sys_crash_t *c)
{
    if (crash_rec.magic != CRASH_MAGIC) return false;
    if (c) *c = crash_rec;
    crash_rec.magic = 0;
    return true;
}

bool sys_peek_crash(sys_crash_t *c) { if (crash_rec.magic != CRASH_MAGIC) return false; *c = crash_rec; return true; }

uint32_t sys_crash_count(void) { return (crash_count_v & 0xFFFF0000u) == 0x5A5A0000u ? crash_count_v & 0xFFFF : 0; }

__attribute__((weak)) void ndf_on_panic(void) {}

static const char *const rname[32] = { "pc", "ra", "sp", "gp", "tp", "t0", "t1", "t2", "s0", "s1", "a0", "a1", "a2", "a3",
    "a4", "a5", "a6", "a7", "s2", "s3", "s4", "s5", "s6", "s7", "s8", "s9", "s10", "s11", "t3", "t4", "t5", "t6" };

/* frame: the 32-word trap frame of vectors.S (_trap_vec): [0] pc, [n] xn */
void ndf_panic_report(const char *why, uint32_t pc, const uint32_t *frame, uint32_t sp)
{
    irq_lock();
    ndf_on_panic();
    int core = core_id();
    printf("\n*** PANIC core%d: %s\n    PC 0x%08lx\n", core, why, (unsigned long)pc);
    crash_rec.magic = 0; crash_rec.core = (uint8_t)core; crash_rec.pc = pc; crash_rec.n_bt = 0;
    crash_rec.uptime_ms = (uint32_t)(time_us() / 1000);
    { int i = 0; for (; why[i] && i < (int)sizeof crash_rec.why - 1; i++) crash_rec.why[i] = why[i]; crash_rec.why[i] = 0; }
    crash_count_v = (crash_count_v & 0xFFFF0000u) == 0x5A5A0000u ? crash_count_v + 1 : 0x5A5A0001u;
    if (frame) {
        for (int r = 1; r < 32; r++)
            printf("    %-3s 0x%08lx%s", rname[r], (unsigned long)frame[r], (r & 3) == 3 ? "\n" : "");
        printf("\n");
    }
    printf("    Backtrace:");
    uint32_t here_sp;
    __asm__ volatile("mv %0, sp" : "=r"(here_sp));
    if (!sp) sp = here_sp;
    if (!ndf_unwind(pc, sp, frame ? frame[1] : 0, 0, emit_pc)) {
        uint32_t top = core ? (uint32_t)_stack1_top : (uint32_t)_stack0_top;
        if (sp >= top || top - sp > 0x20000) top = sp + 1024;
        emit_pc(pc);
        if (frame && is_ret_addr(frame[1])) emit_pc(frame[1]);
        int n = 0;
        for (uint32_t a = sp & ~3u; a < top && n < 24; a += 4) {
            uint32_t w = *(const volatile uint32_t *)a;
            if (is_ret_addr(w)) { emit_pc(w); n++; }
        }
        printf(" (scan)");
    }
    printf("\n");
    crash_rec.magic = CRASH_MAGIC;
}

NORETURN void ndf_panic(const char *why, uint32_t pc, const uint32_t *frame, uint32_t sp)
{
    ndf_panic_report(why, pc, frame, sp);
#if NDF_PANIC_RESTART
    static RTC_NOINIT_ATTR uint32_t early;
    early = time_us() < 5000000 ? ((early & 0xFFFF0000u) == 0x45410000u ? early + 1 : 0x45410001u) : 0;
    if ((early & 0xFFFF) >= 3) { printf("*** crash loop: halted (power-cycle or reflash)\n"); early = 0; for (;;) {} }
    delay_us(20000);
    sys_restart();
#else
    wdt_off();                                            /* halt means halt: no watchdog reset storm */
    for (;;) {}
#endif
}

/* kind 1: exception (mcause < 16), 2: interrupt that was not CLIC-vectored */
NORETURN void ndf_fault(uint32_t kind, uint32_t cause, uint32_t pc, uint32_t vaddr, uint32_t *frame, uint32_t sp)
{
    static const char *c[] = { "instruction address misaligned", "instruction access fault", "illegal instruction (FPU in an ISR?)",
                               "breakpoint", "load address misaligned", "load access fault", "store address misaligned",
                               "store access fault", "ecall from U-mode", "?", "?", "ecall from M-mode" };
    char why[128];
    if (kind == 2) snprintf(why, sizeof why, "unexpected non-vectored interrupt, id %lu", (unsigned long)cause);
    else snprintf(why, sizeof why, "exception, mcause %lu (%s), mtval 0x%08lx", (unsigned long)cause,
                  cause < sizeof c / sizeof c[0] ? c[cause] : "?", (unsigned long)vaddr);
    ndf_panic(why, pc, frame, frame ? frame[2] : sp);
}

NORETURN void ndf_assert_fail(const char *file, int line, const char *cond)
{
    char why[128];
    uint32_t sp;
    __asm__ volatile("mv %0, sp" : "=r"(sp));
    snprintf(why, sizeof why, "assert %s:%d: %s", file, line, cond);
    ndf_panic(why, (uint32_t)__builtin_return_address(0) - 4, 0, sp);
}

uint8_t ndf_panic_stack[2][2048] __attribute__((aligned(16)));
