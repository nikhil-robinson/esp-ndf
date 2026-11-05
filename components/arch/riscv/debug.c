/* RISC-V profiling and debug (debug.h): performance counters on mcycle / minstret, data watchpoints on the trigger module
 * (Debug spec 0.13 mcontrol: tselect 0x7A0, tdata1 0x7A1, tdata2 0x7A2; ESP-IDF v6.1 esp_cpu_set_watchpoint). A hit is a
 * breakpoint exception (mcause 3) taken before the access: ndf_fault (soc sys.c) calls ndf_watch_hit, which clears the
 * trigger (one-shot, so the instruction then completes) and runs fn(n, pc); vectors.S resumes after ndf_fault returns.
 * With a debugger attached the triggers belong to it (dmode): the writes are ignored and watch_set reports -1. */
#include "debug.h"
#include "cpu.h"

#define WATCHES 3
static int ntrig(void)                                      /* triggers the core implements: tselect reads back what was written */
{
    int n = 0;
    for (; n < WATCHES; n++) { __asm__ volatile("csrw 0x7A0, %0" :: "r"(n)); uint32_t r; __asm__ volatile("csrr %0, 0x7A0" : "=r"(r)); if (r != (uint32_t)n) break; }
    return n;
}
#define CSR_TSELECT 0x7A0
#define CSR_TDATA1  0x7A1
#define CSR_TDATA2  0x7A2
#define RD(csr) ({ uint32_t v_; __asm__ volatile("csrr %0, " #csr : "=r"(v_)); v_; })
#define WR(csr, v) __asm__ volatile("csrw " #csr ", %0" :: "r"((uint32_t)(v)))

static watch_fn_t watch_fn[SOC_CPU_CORES][WATCHES];
static uint32_t ev[2];

void perf_setup(int n, uint32_t event) { if (n == 0 || n == 1) ev[n] = event; }
void perf_start(void) {}
void perf_stop(void) {}
uint32_t perf_read(int n)
{
    if (n < 0 || n > 1) return 0;
    return ev[n] == PERF_CYCLES ? RD(mcycle) : ev[n] == PERF_INSN ? RD(minstret) : 0;
}
void perf_reset(int n)
{
    if (n == 0 || n == 1) { if (ev[n] == PERF_CYCLES) WR(mcycle, 0); else if (ev[n] == PERF_INSN) WR(minstret, 0); }
}

int watch_set(int n, const volatile void *addr, size_t len, int mode, watch_fn_t fn)
{
    if (n < 0 || n >= WATCHES || n >= ntrig() || !len || len > 64 || (len & (len - 1)) || ((uint32_t)addr & (len - 1)) || !(mode & 3)) return -1;
    uint32_t t1 = 2u << 28 | BIT(6) | (mode & WATCH_WRITE ? BIT(1) : 0) | (mode & WATCH_READ ? BIT(0) : 0);   /* type 2, M-mode, store / load */
    uint32_t t2 = (uint32_t)addr;
    if (len > 1) { t1 |= 1u << 7; t2 |= (len >> 1) - 1; }                                                     /* match 1: NAPOT range */
    WR(0x7A5, BIT(7) | BIT(3));                                                                               /* tcontrol: M-mode triggers enabled (MPTE | MTE), as IDF rv_utils */
    WR(0x7A0, n);
    WR(0x7A1, 0);
    WR(0x7A2, t2);
    WR(0x7A1, t1);
    if ((RD(0x7A1) & ~(0x3Fu << 21 | BIT(20))) != t1) { WR(0x7A1, 0); return -1; }                                                         /* not writable: a debugger owns it (maskmax, bits 26:21, and hit are read-only) */
    watch_fn[core_id()][n] = fn;
    return 0;
}

void watch_clear(int n)
{
    if (n < 0 || n >= WATCHES) return;
    WR(0x7A0, n);
    WR(0x7A1, 0);
}

/* ndf_fault, breakpoint exception: true when a watchpoint caused it and fn handled it (the caller then resumes) */
bool ndf_watch_hit(uint32_t pc)
{
    for (int n = 0; n < WATCHES; n++) {
        WR(0x7A0, n);
        uint32_t t1 = RD(0x7A1);
        if ((t1 >> 28) != 2 || !(t1 & BIT(20))) continue;                 /* not an enabled mcontrol trigger with its hit bit */
        watch_fn_t fn = watch_fn[core_id()][n];
        watch_clear(n);
        if (!fn) return false;                                             /* no handler: panic with registers */
        fn(n, pc);
        return true;
    }
    return false;
}
