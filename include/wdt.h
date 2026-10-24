/* Watchdogs. wdt_*: timer group 0 MWDT; wdt1_*: timer group 1 (same behaviour, second independent timer);
 * rtc_wdt_*: RTC watchdog (RTC slow clock: also runs through light/deep sleep wake-up paths; chip_reset resets the RTC
 * domain too); swd_*: super watchdog (fixed ~1.6 s timeout, auto-fed unless enabled).
 *
 * wdt_* (timer group 0 MWDT). Feed it from the main loop; if the loop
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

int      wdt1_start(uint32_t timeout_ms, bool report);   /* timer group 1, same semantics (cause TG1 watchdog) */
void     wdt1_feed(void);
void     wdt1_stop(void);
int      rtc_wdt_start(uint32_t timeout_ms, bool chip_reset);   /* cause RTC watchdog (system) or (RTC) */
void     rtc_wdt_feed(void);
void     rtc_wdt_stop(void);
void     swd_enable(bool on);                    /* on: swd_feed() needed within the timeout */
void     swd_feed(void);
