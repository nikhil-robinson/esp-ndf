/* RTC IO: GPIO0..21 (S3) / GPIO0..15 (P4) driven by the RTC (low-power) domain, plus pad hold.
 * Hold freezes a pad's state (level, direction, pulls) until released and
 * survives a software reset, e.g. to keep a motor driver disabled while the
 * CPU reboots. Flags are the gpio.h GPIO_* flags. */
#pragma once
#include "gpio.h"

/* Route the pad to RTC IO; -1 if not 0..21 (S3) / 0..15 (P4). The RTC output latch keeps its
 * last value across resets: rtcio_write() the wanted level BEFORE
 * rtcio_config(..GPIO_OUTPUT) or the pad briefly drives the stale level. */
int  rtcio_config(int pin, uint32_t flags);
void rtcio_release(int pin);                  /* back to the digital GPIO matrix */
void rtcio_write(int pin, bool v);
bool rtcio_read(int pin);
void rtcio_drive(int pin, int strength);      /* 0..3 */
void rtcio_hold(int pin, bool on);            /* works in RTC and digital mode */
bool rtcio_held(int pin);
