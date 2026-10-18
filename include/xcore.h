/* Sharing data between the two cores, cheapest first.
 *
 * Internal SRAM is not cached on the S3: a store by one core is seen by the
 * other on its next load. Only ordering needs care (memw).
 *
 * 1. One aligned 32-bit value: plain volatile load/store, never torn.
 * 2. A struct: xc_seq (sequence counter), writer never waits, reader retries
 *    only if it overlapped a write. Exactly one writer. The writer must not
 *    rewrite faster than the reader can copy (64 B copy ~70 cycles): a writer
 *    looping back to back starves the reader. Any loop under ~3 MHz is fine.
 * 2b. A stream of words: xc_fifo (single producer, single consumer ring).
 * 3. Rare shared state with several writers: xc_spin (S32C1I compare-and-swap).
 * 4. "Act now" on the other core: xc_notify (cross-core interrupt through
 *    the interrupt matrix). Round trip 152 / 127 / 104 cycles at level
 *    1 / 3 / 5: use level 5 when latency matters. Polling a word is faster
 *    (30-49 cycles) but needs the receiver to check; the interrupt does not. */
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

/* ---- SPSC FIFO: one producer core, one consumer core ------------------ */
#define XC_FIFO(name, size_pow2) struct { volatile uint32_t head; uint32_t _p0[7]; \
                                           volatile uint32_t tail; uint32_t _p1[7]; uint32_t buf[size_pow2]; } name
#define xc_fifo_size(f) (sizeof((f)->buf) / 4)
#define xc_fifo_push(f, v) ({ uint32_t _h = (f)->head; bool _ok = _h - (f)->tail < xc_fifo_size(f); \
        if (_ok) { (f)->buf[_h & (xc_fifo_size(f) - 1)] = (v); xc_barrier(); (f)->head = _h + 1; } _ok; })
#define xc_fifo_pop(f, out) ({ uint32_t _t = (f)->tail; bool _ok = _t != (f)->head; \
        if (_ok) { *(out) = (f)->buf[_t & (xc_fifo_size(f) - 1)]; xc_barrier(); (f)->tail = _t + 1; } _ok; })

/* ---- spinlock ------------------------------------------------------------- */
#if SOC_CPU_HAS_S32C1I
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
#else
/* No compare-and-swap: Peterson's algorithm for two cores (plain loads and
 * stores + barriers; correct because each core only writes its own flag). */
typedef struct { volatile uint32_t want[2], turn, owner; } xc_spin_t;

static inline bool xc_spin_try(xc_spin_t *l)
{
    int me = core_id(), other = !me;
    l->want[me] = 1; xc_barrier();
    l->turn = other; xc_barrier();
    if (l->want[other] && l->turn == (uint32_t)other) { l->want[me] = 0; return false; }
    l->owner = me + 1;
    return true;
}
static inline void xc_spin_lock(xc_spin_t *l)
{
    int me = core_id(), other = !me;
    l->want[me] = 1; xc_barrier();
    l->turn = other; xc_barrier();
    while (l->want[other] && l->turn == (uint32_t)other) ;
    l->owner = me + 1;
}
static inline void xc_spin_unlock(xc_spin_t *l) { l->owner = 0; xc_barrier(); l->want[core_id()] = 0; }
#endif

/* ---- cross-core interrupt --------------------------------------------- */
#if SOC_HAS_CROSS_CORE_INT
/* Install fn on the calling core for notifications sent to it (level 1-3). */
int  xc_notify_init(void (*fn)(void *), void *arg, int level);
/* Interrupt the other core (core = target). */
static inline void xc_notify(int core) { REG(0x600C0030 + 4 * core) = 1; }
#endif /* SOC_HAS_CROSS_CORE_INT */
