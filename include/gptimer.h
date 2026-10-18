/* General-purpose timers: 4 x 54-bit up/down counters (TIMG0/1 x T0/T1),
 * APB (80 MHz) or XTAL (40 MHz) source, 2..65536 prescaler, alarm + IRQ. */
#pragma once
#include "ndf.h"
#include "irq.h"

#define GPT_COUNT 4

/* Returns the actual resolution in Hz (exact divider of the source), 0 on error.
 * Source: APB when resolution divides 80 MHz evenly, else XTAL if that does. */
uint32_t gpt_init(int id, uint32_t resolution_hz);
void     gpt_start(int id);
void     gpt_stop(int id);
void     gpt_set(int id, uint64_t count);
uint64_t gpt_get(int id);      /* ~110 cycles: hardware latch (~50) + two reads; at the floor */
uint32_t gpt_get32(int id);    /* low 32 bits, one read less. For CPU-local timing use ccount() (1 cycle) */

/* Alarm at `alarm` ticks; periodic: reload to 0 and re-arm in hardware path.
 * fn runs in the timer ISR at `level` on the calling core (NULL: no IRQ). */
int      gpt_alarm(int id, uint64_t alarm, bool periodic, irq_fn_t fn, void *arg, int level);
void     gpt_alarm_off(int id);
