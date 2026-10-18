/* Interrupt API. Handlers run with the FPU disabled (float use = fault).
 * Levels 1..5; higher preempts lower. Level 4/5 for hard-real-time paths. */
#pragma once
#include "ndf.h"

typedef void (*irq_fn_t)(void *arg);

/* Route peripheral `src` to a free CPU interrupt of `level` on the calling
 * core, install `fn`, enable it. Returns the CPU interrupt number or -1.
 * When a level 1-3 has no free line left, the source shares the least-used
 * line of that level; handlers must check their own status (all esp-ndf
 * drivers do). Levels 4-5 are never shared. */
int  irq_attach(int src, int level, irq_fn_t fn, void *arg);
void irq_detach(int src, int cpu_int);

/* Install fn on a CPU interrupt directly (internal ones: CCOMPARE timers,
 * software ints) on the calling core and enable it. */
void irq_install(int cpu_int, irq_fn_t fn, void *arg);
