/* Software timers: any number of one-shot / periodic callbacks multiplexed on
 * one SYSTIMER comparator (16 MHz, 62.5 ns). Deadlines are absolute, so a
 * periodic timer never drifts: period n fires at start + n * period exactly.
 * Callbacks run in the timer ISR (level NDF_SWTIMER_LEVEL) on the core that
 * started the first timer. Timer structs are owned by the caller (no heap). */
#pragma once
#include "ndf.h"

typedef struct swtimer {
    uint64_t when;                       /* next deadline, ticks */
    uint32_t period;                     /* ticks, 0 = one-shot */
    void (*fn)(void *arg);
    void *arg;
    struct swtimer *next;
    bool active;
} swtimer_t;

/* (Re)start: first call after `us`, then every `us` if periodic. Safe from
 * ISRs, callbacks (including its own) and either core. */
void swt_start(swtimer_t *t, uint32_t us, bool periodic, void (*fn)(void *), void *arg);
void swt_start_ticks(swtimer_t *t, uint64_t first_at, uint32_t period, void (*fn)(void *), void *arg);
void swt_stop(swtimer_t *t);
static inline bool swt_active(const swtimer_t *t) { return t->active; }
