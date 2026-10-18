/* Sharing data between the two cores, cheapest first.
 *
 * Internal SRAM is not cached on the S3: a store by one core is seen by the
 * other on its next load. Only ordering needs care (memw).
 *
 * 1. One aligned 32-bit value: plain volatile load/store, never torn.
 * 2. A struct: xc_seq (sequence counter), writer never waits, reader retries
 *    only if it overlapped a write. Exactly one writer.
 * 3. Rare shared state with several writers: xc_spin (S32C1I compare-and-swap).
 * 4. "Act now" on the other core: xc_notify (cross-core interrupt). */
#pragma once
#include "ndf.h"
#include "xtensa.h"

static inline void xc_barrier(void) { __asm__ volatile("memw" ::: "memory"); }

/* ---- sequence counter --------------------------------------------------- */
typedef struct { volatile uint32_t seq; } xc_seq_t;

static inline void xc_seq_write_begin(xc_seq_t *s) { s->seq++; xc_barrier(); }
static inline void xc_seq_write_end(xc_seq_t *s)   { xc_barrier(); s->seq++; }
/* reader: do { v = xc_seq_read_begin(s); copy; } while (xc_seq_read_retry(s, v)); */
static inline uint32_t xc_seq_read_begin(const xc_seq_t *s)
{
    uint32_t v;
    while ((v = s->seq) & 1) ;                     /* writer in progress */
    xc_barrier();
    return v;
}
static inline bool xc_seq_read_retry(const xc_seq_t *s, uint32_t v) { xc_barrier(); return s->seq != v; }

/* ---- spinlock ------------------------------------------------------------- */
typedef struct { volatile uint32_t owner; } xc_spin_t;   /* 0 = free, else core + 1 */

static inline bool xc_spin_try(xc_spin_t *l)
{
    uint32_t v = core_id() + 1;
    __asm__ volatile("wsr %1, scompare1\n s32c1i %0, %2, 0"
                     : "+r"(v) : "r"(0), "r"(&l->owner) : "memory");
    return v == 0;                                 /* old value: 0 means we took it */
}
static inline void xc_spin_lock(xc_spin_t *l)   { while (!xc_spin_try(l)) ; }
static inline void xc_spin_unlock(xc_spin_t *l) { xc_barrier(); l->owner = 0; }

/* ---- cross-core interrupt --------------------------------------------- */
/* Install fn on the calling core for notifications sent to it (level 1-3). */
int  xc_notify_init(void (*fn)(void *), void *arg, int level);
/* Interrupt the other core (core = target). */
static inline void xc_notify(int core) { REG(0x600C0030 + 4 * core) = 1; }
