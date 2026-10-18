/* Boundary-tag heap. Block = 16-byte header (size incl. header, prev size,
 * free flag) + payload; blocks tile the region, so neighbours are found by
 * address and merged in O(1) on free.
 * ponytail: first fit is O(blocks) per malloc; segregated free lists if
 * allocation-heavy code shows up in profiles. */
#include "heap.h"
#include "xcore.h"
#include "xtensa.h"
#include <string.h>

typedef struct { uint32_t size, prev, used, _pad; } blk_t;   /* 16 bytes keeps payload aligned */

extern char _stack1_top[];
#define HEAP_END ((char *)0x3FCE9000)              /* ROM loader data above (see link.ld) */

static char *base;
static size_t nfree, minfree;
static xc_spin_t lock;

#define NEXT(b) ((blk_t *)((char *)(b) + (b)->size))
#define PREV(b) ((blk_t *)((char *)(b) - (b)->prev))

static void init(void)
{
    base = (char *)(((uintptr_t)_stack1_top + 15) & ~15u);
    blk_t *b = (blk_t *)base;
    b->size = (uint32_t)(HEAP_END - base - sizeof(blk_t)) & ~15u;
    b->prev = 0; b->used = 0;
    blk_t *end = NEXT(b);                          /* sentinel: used, size 0 */
    end->size = 0; end->prev = b->size; end->used = 1;
    nfree = minfree = b->size - sizeof(blk_t);
}

static inline uint32_t lock_all(void) { uint32_t ps = irq_lock(); xc_spin_lock(&lock); if (!base) init(); return ps; }
static inline void unlock_all(uint32_t ps) { xc_spin_unlock(&lock); irq_unlock(ps); }

void *malloc(size_t n)
{
    if (!n || n > 0x7FFFFFF0) return 0;
    uint32_t need = ((n + 15) & ~15u) + sizeof(blk_t);
    uint32_t ps = lock_all();
    for (blk_t *b = (blk_t *)base; b->size; b = NEXT(b)) {
        if (b->used || b->size < need) continue;
        if (b->size - need >= 2 * sizeof(blk_t)) {   /* split off the tail */
            blk_t *r = (blk_t *)((char *)b + need);
            r->size = b->size - need; r->prev = need; r->used = 0;
            NEXT(r)->prev = r->size;
            b->size = need;
        }
        b->used = 1;
        nfree -= b->size;
        if (nfree < minfree) minfree = nfree;
        unlock_all(ps);
        return b + 1;
    }
    unlock_all(ps);
    return 0;
}

void free(void *p)
{
    if (!p) return;
    blk_t *b = (blk_t *)p - 1;
    uint32_t ps = lock_all();
    b->used = 0;
    nfree += b->size;
    blk_t *n = NEXT(b);
    if (!n->used) { b->size += n->size; NEXT(b)->prev = b->size; }
    if (b->prev && !PREV(b)->used) { blk_t *pb = PREV(b); pb->size += b->size; NEXT(pb)->prev = pb->size; }
    unlock_all(ps);
}

void *calloc(size_t n, size_t size)
{
    if (size && n > 0x7FFFFFF0 / size) return 0;
    void *p = malloc(n * size);
    if (p) memset(p, 0, n * size);
    return p;
}

void *realloc(void *p, size_t n)
{
    if (!p) return malloc(n);
    if (!n) { free(p); return 0; }
    size_t have = ((blk_t *)p - 1)->size - sizeof(blk_t);
    if (n <= have) return p;
    void *q = malloc(n);
    if (q) { memcpy(q, p, have); free(p); }
    return q;
}

size_t heap_free(void) { uint32_t ps = lock_all(); size_t f = nfree; unlock_all(ps); return f; }
size_t heap_min_free(void) { uint32_t ps = lock_all(); size_t f = minfree; unlock_all(ps); return f; }

size_t heap_largest(void)
{
    uint32_t ps = lock_all(); size_t m = 0;
    for (blk_t *b = (blk_t *)base; b->size; b = NEXT(b))
        if (!b->used && b->size - sizeof(blk_t) > m) m = b->size - sizeof(blk_t);
    unlock_all(ps);
    return m;
}
