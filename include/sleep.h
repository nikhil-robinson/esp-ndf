/* Sleep (TRM 10; ESP-IDF esp_sleep / rtc_sleep.c). Deep sleep: everything but the RTC domain is off; the wake is a
 * reset (cause RESET_DEEPSLEEP) through the ROM, RTC_DATA_ATTR / RTC_BSS_ATTR keep their values. NOT YET VERIFIED ON
 * HARDWARE (docs/RESULTS.md): run only with the logic analyzer watching.
 * Wake sources: RTC timer, EXT0 (one RTC GPIO at a level), EXT1 (a set of RTC GPIOs: any high, or all low). */
#pragma once
#include "ndf.h"

enum { WAKE_EXT0 = 1u << 0, WAKE_EXT1 = 1u << 1, WAKE_GPIO = 1u << 2, WAKE_TIMER = 1u << 3 };
void     sleep_wake_timer(uint64_t us);
int      sleep_wake_ext0(int rtc_pin, int level);            /* RTC GPIO 0..21 */
int      sleep_wake_ext1(uint32_t rtc_pin_mask, bool any_high);
void     sleep_wake_none(void);
NORETURN void sleep_deep(void);
/* Light sleep until a wake source fires: RAM, CPU state and peripherals kept, returns the WAKE_* cause (0 = the
 * hardware rejected the sleep, or no wake source set). The USB Serial/JTAG link drops while asleep (host re-plug). */
uint32_t sleep_light(void);
uint64_t sleep_rtc_us(void);                                 /* RTC timer, runs through every sleep */
uint32_t sleep_wake_cause(void);                             /* WAKE_* bits of the last wake (after the reset) */
uint32_t sleep_ext1_pins(void);                              /* EXT1 pins that woke the chip */
