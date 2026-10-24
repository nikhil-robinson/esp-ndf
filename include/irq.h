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
/* Non-maskable (level 7) handler for one source on this core; fn NULL = off. Not blocked by irq_lock(); no FPU/PIE. */
int  irq_attach_nmi(int src, irq_fn_t fn, void *arg);

/* Install fn on a CPU interrupt directly (internal ones: CCOMPARE timers,
 * software ints) on the calling core and enable it. */
void irq_install(int cpu_int, irq_fn_t fn, void *arg);

/* Naked level-4/5 handler (assembly): entered with only a0 free (original
 * in EXCSAVEn); saves what it uses, clears its source, returns with
 * "rsr a0, excsaveN; rfi N". No C, no register frame: the floor latency.
 * Each core has its own vector table: this patches the calling core's
 * level-N vector only (a direct jump when the handler is within 128 KB).
 * One naked handler per level per core. Returns the CPU interrupt or -1. */
int  irq_naked(int src, int level, void (*handler)(void));
void irq_naked_install(int cpu_int, void (*handler)(void));
