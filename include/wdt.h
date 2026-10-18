/* Watchdog (timer group 0 MWDT). Feed it from the main loop; if the loop
 * stalls for `timeout_ms`:
 *   report = true:  a level-3 interrupt prints where the core was stuck (PC +
 *                   backtrace, like a fault), then the second stage resets
 *                   the chip `timeout_ms` later;
 *   report = false: the chip resets at once.
 * After the reset sys_reset_cause() reads "TG0 watchdog (system)".
 * A core stuck with interrupts masked (irq_lock) skips the report and is
 * still reset. */
#pragma once
#include "ndf.h"

int  wdt_start(uint32_t timeout_ms, bool report);   /* 1 ms .. ~32 s at 80 MHz APB */
void wdt_feed(void);
void wdt_stop(void);
