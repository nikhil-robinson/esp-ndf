/* Boundary-tag heap. Block = 16-byte header (size incl. header, prev size,
 * free flag) + payload; blocks tile the region, so neighbours are found by
 * address and merged in O(1) on free. Regions: internal DRAM (internal, DMA)
 * and PSRAM once heap_add_psram() ran; malloc as IDF (SPIRAM_MALLOC_ALWAYSINTERNAL):
 * below NDF_PSRAM_MALLOC_MIN internal first, larger PSRAM first.
 * ponytail: first fit is O(blocks) per malloc; segregated free lists if
 * allocation-heavy code shows up in profiles. */
#include "heap.h"
#include "xcore.h"
#include "xtensa.h"
#include <string.h>

typedef struct { uint32_t size, prev, used, magic; } blk_t;   /* 16 bytes keeps payload aligned */

/* every header carries magic ^ its own address: a stray free, a double free or an overflow into the next header is
 * caught (panic) on the next free / allocation walk instead of corrupting the heap silently */
#define MAGIC 0x4E44480Bu
#define SEAL(b) ((b)->magic = MAGIC ^ (uint32_t)(uintptr_t)(b))
#define SEALED(b) ((b)->magic == (MAGIC ^ (uint32_t)(uintptr_t)(b)))
void ndf_panic(const char *why, uint32_t pc, const uint32_t *frame, uint32_t sp);
void (*heap_on_corrupt)(const char *why);
static void corrupt(const char *why)               /* a hook (tests) sees it and the operation is refused; else panic */
{
    if (heap_on_corrupt) { heap_on_corrupt(why); return; }
    ndf_panic(why, (uint32_t)(uintptr_t)__builtin_return_address(0), 0, 0);
}

extern char _stack1_top[];
#define HEAP_END ((char *)(NDF_APPTRACE ? 0x3FCE8000 : 0x3FCE9000))   /* ROM loader data / trace blocks above (link.ld RAM_END) */

typedef struct { char *base, *end; size_t nfree, minfree; uint32_t caps; } region_t;
static region_t reg[2];
static int nreg;
static xc_spin_t lock;

#define NEXT(b) ((blk_t *)((char *)(b) + (b)->size))
#define PREV(b) ((blk_t *)((char *)(b) - (b)->prev))

static void region_init(region_t *r, char *start, char *end, uint32_t caps)
{
    r->base = (char *)(((uintptr_t)start + 15) & ~15u); r->end = end; r->caps = caps;
    blk_t *b = (blk_t *)r->base;
    b->size = (uint32_t)(end - r->base - sizeof(blk_t)) & ~15u;
    b->prev = 0; b->used = 0; SEAL(b);
    blk_t *e = NEXT(b);                            /* sentinel: used, size 0 */
    e->size = 0; e->prev = b->size; e->used = 1; SEAL(e);
    r->nfree = r->minfree = b->size - sizeof(blk_t);
}

static void init(void) { region_init(&reg[0], _stack1_top, HEAP_END, HEAP_INTERNAL | HEAP_DMA); nreg = 1; }

static inline uint32_t lock_all(void) { uint32_t ps = irq_lock(); xc_spin_lock(&lock); if (!nreg) init(); return ps; }
static inline void unlock_all(uint32_t ps) { xc_spin_unlock(&lock); irq_unlock(ps); }

static void *reg_alloc(region_t *r, uint32_t need)
{
    for (blk_t *b = (blk_t *)r->base; b->size; b = NEXT(b)) {
        if (!SEALED(b) || (char *)NEXT(b) >= r->end) { corrupt("heap corrupted (bad block header)"); return 0; }
        if (b->used || b->size < need) continue;
        if (b->size - need >= 2 * sizeof(blk_t)) {   /* split off the tail */
            blk_t *t = (blk_t *)((char *)b + need);
            t->size = b->size - need; t->prev = need; t->used = 0; SEAL(t);
            NEXT(t)->prev = t->size;
            b->size = need;
        }
        b->used = 1;
        r->nfree -= b->size;
        if (r->nfree < r->minfree) r->minfree = r->nfree;
        return b + 1;
    }
    return 0;
}

void *heap_caps_malloc(size_t n, uint32_t caps)
{
    if (!n || n > 0x7FFFFFF0) return 0;
    uint32_t need = ((n + 15) & ~15u) + sizeof(blk_t);
    uint32_t ps = lock_all();
    void *p = 0;
    for (int i = 0; i < nreg && !p; i++) if ((reg[i].caps & caps) == caps) p = reg_alloc(&reg[i], need);
    unlock_all(ps);
    return p;
}

void *malloc(size_t n)
{
    if (nreg > 1 && n >= NDF_PSRAM_MALLOC_MIN) {      /* large: PSRAM first (keeps internal RAM for DMA, stacks) */
        void *p = heap_caps_malloc(n, HEAP_SPIRAM);
        if (p) return p;
    }
    void *p = heap_caps_malloc(n, HEAP_INTERNAL);
    return p || nreg < 2 ? p : heap_caps_malloc(n, HEAP_SPIRAM);
}

int heap_add_psram(void *start, size_t size)
{
    uint32_t ps = lock_all();
    int r = -1;
    if (nreg < 2 && size > 4096) { region_init(&reg[1], start, (char *)start + size, HEAP_SPIRAM); nreg = 2; r = 0; }
    unlock_all(ps);
    return r;
}

static region_t *owner(void *p)
{
    for (int i = 0; i < nreg; i++) if ((char *)p > reg[i].base && (char *)p < reg[i].end) return &reg[i];
    return 0;
}

void free(void *p)
{
    if (!p) return;
    blk_t *b = (blk_t *)p - 1;
    uint32_t ps = lock_all();
    region_t *r = owner(p);
    const char *bad = !r || ((uintptr_t)p & 15) || !SEALED(b) ? "free(): not a heap block" : !b->used ? "free(): double free" : 0;
    blk_t *nx = bad ? 0 : NEXT(b);
    if (!bad && ((char *)nx >= r->end || !SEALED(nx) || nx->prev != b->size)) bad = "heap corrupted (overflow past a block)";
    if (!bad && b->prev && !SEALED(PREV(b))) bad = "heap corrupted (bad previous block)";
    if (bad) { unlock_all(ps); corrupt(bad); return; }
    b->used = 0;
    r->nfree += b->size;
    blk_t *n = NEXT(b);
    if (!n->used) { b->size += n->size; NEXT(b)->prev = b->size; n->magic = 0; }        /* absorbed headers lose */
    if (b->prev && !PREV(b)->used) { blk_t *pb = PREV(b); pb->size += b->size; NEXT(pb)->prev = pb->size; b->magic = 0; }   /* their seal */
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
    blk_t *b = (blk_t *)p - 1;
    if (((uintptr_t)p & 15) || !owner(p) || !SEALED(b) || !b->used) { corrupt("realloc(): not a live heap block"); return 0; }
    size_t have = b->size - sizeof(blk_t);
    if (n <= have) return p;                       /* (stays in its region) */
    void *q = malloc(n);
    if (q) { memcpy(q, p, have); free(p); }
    return q;
}

size_t heap_caps_free_size(uint32_t caps)
{
    uint32_t ps = lock_all(); size_t f = 0;
    for (int i = 0; i < nreg; i++) if ((reg[i].caps & caps) == caps) f += reg[i].nfree;
    unlock_all(ps);
    return f;
}
size_t heap_free(void) { return heap_caps_free_size(HEAP_INTERNAL); }
size_t heap_min_free(void) { uint32_t ps = lock_all(); size_t f = reg[0].minfree; unlock_all(ps); return f; }

size_t heap_largest(void)
{
    uint32_t ps = lock_all(); size_t m = 0;
    for (blk_t *b = (blk_t *)reg[0].base; b->size; b = NEXT(b))
        if (!b->used && b->size - sizeof(blk_t) > m) m = b->size - sizeof(blk_t);
    unlock_all(ps);
    return m;
}

/* walk every block of every region: headers sealed, sizes and back links consistent (IDF heap_caps_check_integrity_all) */
bool heap_check(void)
{
    uint32_t ps = lock_all();
    bool ok = true;
    for (int i = 0; i < nreg && ok; i++) {
        uint32_t prev = 0;
        for (blk_t *b = (blk_t *)reg[i].base; ok; b = NEXT(b)) {
            if ((char *)b + sizeof *b > reg[i].end || !SEALED(b) || b->prev != prev || (b->size & 15)) { ok = false; break; }
            if (!b->size) break;                         /* sentinel */
            prev = b->size;
        }
    }
    unlock_all(ps);
    return ok;
}

