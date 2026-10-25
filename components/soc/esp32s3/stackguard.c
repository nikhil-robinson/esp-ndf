/* Stack guard on the ASSIST_DEBUG SP monitor (TRM 3.x): per core SP_MIN / SP_MAX, SP_SPILL_MIN/MAX interrupt
 * (bits 8, 9; ENA + RLS), source 83 shared by both cores; the interrupted core's handler checks its own status. */
#include "debug.h"
#include "irq.h"
#include "xtensa.h"
#include <stdio.h>

#define AD(core, off)   (0x600CE000u + (core) * 0x90u + (off))
#define ENA(c)          AD(c, 0x00)
#define RAW(c)          AD(c, 0x04)
#define RLS(c)          AD(c, 0x08)     /* ENA arms the check, RLS lets it raise the interrupt (IDF assist_debug_ll) */
#define CLR(c)          AD(c, 0x0C)
#define SP_MIN(c)       AD(c, 0x3C)
#define SP_MAX(c)       AD(c, 0x40)
#define SP_PC(c)        AD(c, 0x44)
#define SP_BITS         (BIT(8) | BIT(9))
#define SRC_ASSIST      83

static stack_guard_fn_t guard_fn[SOC_CPU_CORES];
static int8_t guard_line[SOC_CPU_CORES] = { -1, -1 };

void ndf_panic(const char *why, uint32_t pc, const uint32_t *frame, uint32_t sp);

RAM_FN static void guard_isr(void *a)
{
    (void)a;
    int c = core_id();
    if (!(REG(RAW(c)) & SP_BITS)) return;
    uint32_t pc = REG(SP_PC(c));
    uint32_t sp; __asm__ volatile("mov %0, a1" : "=r"(sp));
    if (sp >= REG(SP_MIN(c)) && sp <= REG(SP_MAX(c))) {   /* SP is fine again: a debugger halt (OpenOCD runs code */
        REG(CLR(c)) = SP_BITS;                            /* in debug mode with its own a1), not an overflow - on a */
        return;                                           /* real one this handler still runs on the bad stack */
    }
    REG(ENA(c)) &= ~SP_BITS; REG(RLS(c)) &= ~SP_BITS;
    REG(CLR(c)) = SP_BITS;
    if (guard_fn[c]) { guard_fn[c](c, pc); return; }
    ndf_panic("stack overflow (SP left its stack)", pc, 0, sp);
}

int stack_guard(uint32_t lo, uint32_t hi, stack_guard_fn_t fn)
{
    int c = core_id();
    REG_SET(0x600C0008u, BIT(6));                                   /* SYSTEM_CPU_PERI_CLK_EN: ASSIST_DEBUG */
    REG_CLR(0x600C000Cu, BIT(6));                                   /* SYSTEM_CPU_PERI_RST_EN */
    REG(ENA(c)) &= ~SP_BITS;
    guard_fn[c] = fn;
    REG(SP_MIN(c)) = lo; REG(SP_MAX(c)) = hi;
    REG(CLR(c)) = SP_BITS;
    if (guard_line[c] < 0 && (guard_line[c] = irq_attach(SRC_ASSIST, 3, guard_isr, 0)) < 0) return -1;
    REG(ENA(c)) |= SP_BITS;
    REG(RLS(c)) |= SP_BITS;
    return 0;
}

void stack_guard_core_off(int c) { REG(ENA(c)) &= ~SP_BITS; REG(RLS(c)) &= ~SP_BITS; REG(CLR(c)) = SP_BITS; }
void stack_guard_off(void) { stack_guard_core_off(core_id()); }

/* Move the window without re-arming (context switches: widen to old + new stack, switch, narrow). */
RAM_FN bool stack_guard_bounds(uint32_t lo, uint32_t hi)
{
    int c = core_id();
    if (!(REG(ENA(c)) & SP_BITS)) return false;
    REG(SP_MIN(c)) = lo; REG(SP_MAX(c)) = hi;
    return true;
}

void stack_guard_get(uint32_t *lo, uint32_t *hi) { int c = core_id(); *lo = REG(SP_MIN(c)); *hi = REG(SP_MAX(c)); }
