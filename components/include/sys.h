/* System: clocks, delays, second core, faults. */
#pragma once
#include "ndf.h"

uint32_t sys_cpu_hz(void);
void     delay_us(uint32_t us);             /* busy wait on the cycle counter */
void     delay_ms(uint32_t ms);
uint64_t time_us(void);                     /* microseconds since boot (SYSTIMER, 52-bit, never wraps) */
uint64_t time_ticks(void);                  /* same counter in 16 MHz ticks (62.5 ns) */
static inline uint32_t time_ms(void) { return (uint32_t)(time_us() / 1000); }
uint32_t systimer_lo(void);                 /* free-running 16 MHz counter */

/* Run fn on core 1 (once). Core 1 starts with interrupts masked. */
void     cpu1_start(void (*fn)(void));
bool     sys_cpu1_running(void);              /* cpu1_start was called */

NORETURN void ndf_fault(uint32_t kind, uint32_t cause, uint32_t pc, uint32_t vaddr, uint32_t *frame, uint32_t sp);
/* Fatal-error report: reason, PC, registers (if frame), backtrace from a
 * stack scan (CALL0 has no frame chain: every word on the stack that is a
 * return address right after a call0/callx0 is listed; decode with
 * "make backtrace ADDR='0x... 0x...'"). Then halts or restarts
 * (NDF_PANIC_RESTART). */
NORETURN void ndf_panic(const char *why, uint32_t pc, const uint32_t *frame, uint32_t sp);
/* Called first on every fault / assert / watchdog report, interrupts masked, on the panic stack: drive actuators to a
 * safe state here (register writes only - no heap, no locks, no printf). Weak, default empty. A watchdog firing with
 * interrupts masked is a hardware reset instead: no code runs, every pad returns to its reset state (input, no drive). */
void ndf_on_panic(void);
void ndf_panic_report(const char *why, uint32_t pc, const uint32_t *frame, uint32_t sp);   /* print only, interrupts left masked */
