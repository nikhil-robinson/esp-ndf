/* CPU profiling and debug: performance counters, data watchpoints, hardware stack guard. All per calling core. */
#pragma once
#include "ndf.h"

#if SOC_CPU_RISCV
/* ---- performance counters: 2 per core. The RISC-V HP cores count cycles (mcycle) and retired instructions (minstret); the
 * other events of the Xtensa cores (cache misses, stalls, interrupts) have no counter here: they read 0. ---------------- */
#define PERF_CYCLES       1u
#define PERF_INSN         2u
#define PERF_D_STALL      0u
#define PERF_DCACHE_MISS  0u
#define PERF_I_STALL      0u
#define PERF_ICACHE_MISS  0u
#define PERF_EXR          0u
#define PERF_IRQ          0u
#define PERF_D_LOAD       0u
#define PERF_D_STORE      0u
void perf_setup(int n, uint32_t event);
void perf_start(void);
void perf_stop(void);
uint32_t perf_read(int n);
void perf_reset(int n);
#else
/* ---- performance counters: 2 per core (Xtensa perfmon, ERI registers), 32-bit ---------------------------------
 * perf_setup(0, PERF_INSN); perf_setup(1, PERF_DCACHE_MISS); perf_start(); ... perf_read(0) ...
 * An event is PMCTRL's SELECT (bits 12:8) | MASK (31:16); counted at every interrupt level. */
#define PERF_CYCLES       ((0x0001u << 16) | (0u << 8))
#define PERF_INSN         ((0x8DFFu << 16) | (2u << 8))   /* retired instructions */
#define PERF_D_STALL      ((0x01FEu << 16) | (3u << 8))   /* data-side global stall cycles */
#define PERF_DCACHE_MISS  ((0x0008u << 16) | (3u << 8))   /* ... of them on a dcache miss */
#define PERF_I_STALL      ((0x01FFu << 16) | (4u << 8))   /* instruction-side + other stall cycles */
#define PERF_ICACHE_MISS  ((0x0001u << 16) | (4u << 8))   /* ... of them on an icache miss */
#define PERF_EXR          ((0x01FFu << 16) | (5u << 8))   /* exceptions, interrupts, replays */
#define PERF_IRQ          ((0x0006u << 16) | (5u << 8))   /* interrupts taken (level 1 + higher) */
#define PERF_D_LOAD       ((0x000Fu << 16) | (10u << 8))  /* data memory loads */
#define PERF_D_STORE      ((0x0003u << 16) | (11u << 8))  /* data memory stores */

#define PERF_ERI_PGM      0x101000u
#define PERF_ERI_PM(n)    (0x101080u + 4 * (n))
#define PERF_ERI_CTRL(n)  (0x101100u + 4 * (n))
static inline uint32_t eri_rd(uint32_t a) { uint32_t v; __asm__ volatile("rer %0, %1" : "=r"(v) : "r"(a)); return v; }
static inline void eri_wr(uint32_t a, uint32_t v) { __asm__ volatile("wer %0, %1" :: "r"(v), "r"(a) : "memory"); }
static inline void perf_setup(int n, uint32_t event) { eri_wr(PERF_ERI_PM(n), 0); eri_wr(PERF_ERI_CTRL(n), event | 15u << 4); }   /* KRNLCNT 0: count while CINTLEVEL <= 15 */
static inline void perf_start(void) { eri_wr(PERF_ERI_PGM, 1); }
static inline void perf_stop(void)  { eri_wr(PERF_ERI_PGM, 0); }
static inline uint32_t perf_read(int n) { return eri_rd(PERF_ERI_PM(n)); }
static inline void perf_reset(int n) { eri_wr(PERF_ERI_PM(n), 0); }
#endif

/* ---- data watchpoints: 2 per core (Xtensa DBREAK) / 3 (RISC-V trigger module) -------------------------------------------------------------------
 * len a power of 2 up to 64, addr aligned to it. On a hit fn(n, pc) runs in the debug exception (level 6) with the
 * watchpoint already cleared (one-shot, so the access then completes); fn NULL = panic with registers. A debugger
 * attached over JTAG takes the hit instead. */
enum { WATCH_READ = 1, WATCH_WRITE = 2 };
typedef void (*watch_fn_t)(int n, uint32_t pc);
int  watch_set(int n, const volatile void *addr, size_t len, int mode, watch_fn_t fn);
void watch_clear(int n);

/* ---- stack guard (ASSIST_DEBUG SP monitor) -------------------------------------------------------------------
 * The hardware compares SP on every change against [lo, hi]; leaving it raises an interrupt: fn(core, pc) (fn NULL
 * = panic "stack overflow"). The guard is then off. NDF_STACK_GUARD arms it at boot on both cores' stacks. */
typedef void (*stack_guard_fn_t)(int core, uint32_t pc);
int  stack_guard(uint32_t lo, uint32_t hi, stack_guard_fn_t fn);
void stack_guard_off(void);
bool stack_guard_bounds(uint32_t lo, uint32_t hi);   /* move the window (task switches); false if the guard is off */
void stack_guard_get(uint32_t *lo, uint32_t *hi);
