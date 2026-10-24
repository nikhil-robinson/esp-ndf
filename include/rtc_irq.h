/* RTC_CNTL interrupt (one CPU source for brownout, touch, ULP, RTC WDT, ...): handlers by status bit. */
#pragma once
#include "ndf.h"
#include "irq.h"

/* fn(arg) runs when any bit of `mask` (RTC_CNTL INT_ST layout) is set; it must clear its bits (rtc_irq_clear). */
int  rtc_irq_on(uint32_t mask, irq_fn_t fn, void *arg);
void rtc_irq_off(uint32_t mask);
void rtc_irq_clear(uint32_t mask);
