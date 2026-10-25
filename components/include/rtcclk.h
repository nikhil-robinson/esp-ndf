/* RTC clocks (TRM 7; ESP-IDF rtc_clk.c, rtc_time.c, clk_tree_ll.h, xt_wdt_ll.h). RTC slow clock: internal RC (~136 kHz,
 * the default), 32 kHz crystal or external oscillator (XTAL_32K_P/N = GPIO15/16), or 8 MHz RC / 256. Calibration
 * counts 40 MHz XTAL cycles over N slow cycles (timer group 0). It clocks the RTC timer, RTC watchdog, sleep timers. */
#pragma once
#include "ndf.h"

enum { SLOW_RC = 0, SLOW_XTAL32K = 1, SLOW_RC8M_D256 = 2 };
int      slow_clk_select(int src);          /* -1: that clock does not run (stays on the current one) */
int      slow_clk_src(void);
uint32_t slow_clk_cal_hz(int src, uint32_t cycles);   /* measured frequency of src, 0 if it does not run */
uint32_t rtc_slow_hz(void);                 /* the current slow clock, calibrated (cached until a source change) */
int      xtal32k_start(bool external_osc);  /* crystal on GPIO15/16, or an oscillator into GPIO15; -1 if no clock */
void     xtal32k_wdt(bool on);              /* the 32 kHz clock stops: switch to the RC backup by itself */
void     rtc_fast_clk_rc(bool rc);          /* RTC fast clock: true 8 MHz RC, false XTAL / 2 (default) */
