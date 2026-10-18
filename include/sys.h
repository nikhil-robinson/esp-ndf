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

NORETURN void ndf_fault(uint32_t kind, uint32_t cause, uint32_t pc, uint32_t vaddr);
