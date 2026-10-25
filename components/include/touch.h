/* Capacitive touch: pads 1..14 = GPIO1..14, hardware scan FSM, raw counts
 * (higher = more capacitance on S3). */
#pragma once
#include "ndf.h"

int      touch_init(uint16_t pad_mask);       /* bit n = pad n; starts scanning */
uint32_t touch_read(int pad);                 /* latest raw count */

/* Hardware filter (IDF defaults: IIR 16, debounce 1, jitter 4, smooth IIR 2) + baseline (benchmark). */
void     touch_filter(bool on);
uint32_t touch_benchmark(int pad);            /* baseline, tracks slow drift */
uint32_t touch_smooth(int pad);               /* filtered value */
/* Active while smooth - benchmark > thr (raw counts; ~2-5 % of the baseline for a finger). */
void     touch_threshold(int pad, uint32_t thr);
typedef void (*touch_fn_t)(int pad, bool touched, void *arg);
int      touch_on_event(touch_fn_t fn, void *arg);   /* from the touch active/inactive interrupt; NULL = off */
uint16_t touch_active(void);                  /* bit n = pad n touched now */
/* Proximity: up to 3 pads (0 = unused) accumulate meas_times scans; count = scans done. */
void     touch_proximity(const int8_t pads[3], uint32_t meas_times);
uint32_t touch_proximity_count(int i);
/* Denoise: pad 0 against an internal reference cap, subtracted from all pads (cancels supply noise).
 * grade 0..3 = 12/10/8/4-bit range, cap 0..7 = 5.0 + 1.4 * cap pF; grade < 0 = off. Set it before touch_init
 * (the scanner stalls if denoise changes while it runs, as in IDF). */
void     touch_denoise(int grade, int cap);
uint32_t touch_denoise_data(void);                /* the reference count being subtracted */
