/* GDMA (TRM ch. 3). Channel register blocks are 0xC0 apart. */
#include "gdma.h"
#include "clk.h"
#include "xtensa.h"
#include "irq.h"
#include "psram.h"

#define G_BASE           0x6003F000
#define IN_CONF0(c)      (G_BASE + 0xC0 * (c) + 0x00)   /* b0 rst b2/b3 burst b4 mem_trans */
#define IN_CONF1(c)      (G_BASE + 0xC0 * (c) + 0x04)   /* b12 check owner */
#define IN_INT_RAW(c)    (G_BASE + 0xC0 * (c) + 0x08)   /* b0 done b1 suc_eof b3 dscr_err */
#define IN_INT_CLR(c)    (G_BASE + 0xC0 * (c) + 0x14)
#define IN_LINK(c)       (G_BASE + 0xC0 * (c) + 0x20)   /* b19:0 addr b20 auto_ret b21 stop b22 start */
#define IN_PRI(c)        (G_BASE + 0xC0 * (c) + 0x44)   /* 0..5, higher wins */
#define IN_PERI(c)       (G_BASE + 0xC0 * (c) + 0x48)
#define OUT_CONF0(c)     (G_BASE + 0xC0 * (c) + 0x60)   /* b0 rst b2 auto_wrback b3 eof_mode b4/b5 burst */
#define OUT_CONF1(c)     (G_BASE + 0xC0 * (c) + 0x64)
#define OUT_INT_RAW(c)   (G_BASE + 0xC0 * (c) + 0x68)   /* b0 done b1 eof b3 total_eof */
#define OUT_INT_CLR(c)   (G_BASE + 0xC0 * (c) + 0x74)
#define OUT_LINK(c)      (G_BASE + 0xC0 * (c) + 0x80)   /* b19:0 addr b20 stop b21 start */
#define OUT_PRI(c)       (G_BASE + 0xC0 * (c) + 0xA4)
#define OUT_PERI(c)      (G_BASE + 0xC0 * (c) + 0xA8)
#define CONF1_OWNER      BIT(12)                        /* IN/OUT_CONF1: check descriptor owner */
#define CONF1_BK(v)      ((uint32_t)(v) << 13)          /* external memory block: 0 16 B, 1 32 B, 2 64 B */
#define IN_DSCR_ERR      BIT(3)
#define OUT_DSCR_ERR     BIT(2)
#define MISC_CONF        (G_BASE + 0x3C8)               /* b0 ahbm rst b4 clk_en */
#define IN_INT_ST(c)     (G_BASE + 0xC0 * (c) + 0x0C)
#define IN_INT_ENA(c)    (G_BASE + 0xC0 * (c) + 0x10)
#define IN_EOF_DES(c)    (G_BASE + 0xC0 * (c) + 0x28)   /* last in descriptor with suc_eof */
#define IN_DSCR_BF0(c)   (G_BASE + 0xC0 * (c) + 0x30)
#define OUT_INT_ST(c)    (G_BASE + 0xC0 * (c) + 0x6C)
#define OUT_INT_ENA(c)   (G_BASE + 0xC0 * (c) + 0x70)
#define OUT_EOF_DES(c)   (G_BASE + 0xC0 * (c) + 0x88)   /* last out descriptor with eof */
#define SRC_IN(c)        (66 + (c))
#define SRC_OUT(c)       (71 + (c))

static uint8_t used;
/* Channel bookkeeping (used, peri_of, m2m_conn) is shared by both cores - crypto takes a channel per operation on
 * whichever core runs it: a spinlock with interrupts off on this core (a non-atomic "used |= BIT(c)" once handed the
 * same channel to SPI on core 0 and AES on core 1). */
static volatile uint32_t gk;
static uint32_t glock(void) { uint32_t ps = irq_lock(); while (__atomic_exchange_n(&gk, 1, __ATOMIC_ACQUIRE)) {} return ps; }
static void gunlock(uint32_t ps) { __atomic_store_n(&gk, 0, __ATOMIC_RELEASE); irq_unlock(ps); }
static bool ext(const void *p) { uint32_t a = (uint32_t)p; return a >= 0x3C000000u && a < 0x3E000000u; }   /* PSRAM data window */

int gdma_alloc(void)
{
    uint32_t ps = glock();
    if (!used) {
        periph_enable(PERIPH_GDMA);
        REG_SET(MISC_CONF, BIT(4));
        REG_SET(MISC_CONF, BIT(0)); REG_CLR(MISC_CONF, BIT(0));
    }
    int r = -1;
    for (int c = 0; c < 5 && r < 0; c++) if (!(used & BIT(c))) { used |= BIT(c); r = c; }
    gunlock(ps);
    return r;
}

static int8_t peri_of[5] = { -1, -1, -1, -1, -1 };     /* peripheral id each channel is connected to */
static uint8_t m2m_conn;                                /* channels connected as memory-to-memory */
void gdma_free(int c)
{
    if (c < 0 || c >= 5) return;
    gdma_stop(c);
    uint32_t ps = glock(); used &= ~BIT(c); peri_of[c] = -1; m2m_conn &= ~BIT(c); gunlock(ps);
}

void gdma_connect(int c, int periph)
{
    if (!(c >= 0 && c < 5)) return;      /* caller index: never past the hardware */
    REG_SET(IN_CONF0(c), BIT(0));  REG_CLR(IN_CONF0(c), BIT(0));
    REG_SET(OUT_CONF0(c), BIT(0)); REG_CLR(OUT_CONF0(c), BIT(0));
    REG(IN_CONF0(c)) = BIT(2) | BIT(3) | (periph == GDMA_M2M ? BIT(4) : 0);
    REG(OUT_CONF0(c)) = BIT(3) | BIT(4) | BIT(5);      /* EOF when data popped by peripheral */
    REG(IN_CONF1(c)) = CONF1_BK(2);                    /* PSRAM in 64-byte blocks (one cache line) */
    REG(OUT_CONF1(c)) = CONF1_BK(2);
    /* M2M must still use a valid peripheral id (0..9: PMS grants SRAM access per peripheral; 10+ is refused with a
     * descriptor error) - one no other channel is connected to (as IDF's m2m_free_periph_mask), else the two would
     * trigger each other (measured: AES DMA on core 1 broke memcpy on core 0 when both used 6) */
    int sel = periph;
    uint32_t ps = glock();
    if (periph == GDMA_M2M) {
        uint32_t busy = 0;
        for (int k = 0; k < 5; k++) if (k != c && peri_of[k] >= 0) busy |= BIT(peri_of[k]);
        sel = -1;
        for (int id = 9; id >= 0 && sel < 0; id--) if (!(busy & BIT(id))) sel = id;
        if (sel < 0) sel = GDMA_AES;                       /* 5 channels, 10 ids: cannot happen */
    }
    peri_of[c] = (int8_t)sel;
    if (periph == GDMA_M2M) m2m_conn |= BIT(c); else m2m_conn &= ~BIT(c);
    gunlock(ps);
    REG(IN_PERI(c)) = sel;
    REG(OUT_PERI(c)) = sel;
}

void gdma_set_priority(int c, int prio) {
    if (!(c >= 0 && c < 5)) return;      /* caller index: never past the hardware */
    REG(IN_PRI(c)) = prio; REG(OUT_PRI(c)) = prio; }

void gdma_check_owner(int c, bool on)
{
    if (!(c >= 0 && c < 5)) return;      /* caller index: never past the hardware */
    if (on) { REG_SET(IN_CONF1(c), CONF1_OWNER); REG_SET(OUT_CONF1(c), CONF1_OWNER); }
    else    { REG_CLR(IN_CONF1(c), CONF1_OWNER); REG_CLR(OUT_CONF1(c), CONF1_OWNER); }
}

int gdma_error(int c)
{
    return (REG(IN_INT_RAW(c)) & IN_DSCR_ERR ? GDMA_ERR_IN : 0) | (REG(OUT_INT_RAW(c)) & OUT_DSCR_ERR ? GDMA_ERR_OUT : 0);
}

void gdma_reset(int c)                                     /* stop, reset both FSMs + FIFOs; configuration kept */
{
    gdma_stop(c);
    REG_SET(IN_CONF0(c), BIT(0));  REG_CLR(IN_CONF0(c), BIT(0));
    REG_SET(OUT_CONF0(c), BIT(0)); REG_CLR(OUT_CONF0(c), BIT(0));
    REG(IN_INT_CLR(c)) = ~0u; REG(OUT_INT_CLR(c)) = ~0u;
}

void gdma_in_reset(int c)  { REG_CLR(IN_LINK(c), BIT(22)); REG_SET(IN_CONF0(c), BIT(0));  REG_CLR(IN_CONF0(c), BIT(0));  REG(IN_INT_CLR(c)) = ~0u; }
void gdma_out_reset(int c) { REG_CLR(OUT_LINK(c), BIT(21)); REG_SET(OUT_CONF0(c), BIT(0)); REG_CLR(OUT_CONF0(c), BIT(0)); REG(OUT_INT_CLR(c)) = ~0u; }

int gdma_chain(gdma_desc_t *d, int n, void *buf, size_t len, bool eof)
{
    uint8_t *p = buf;
    int i = 0;
    if (ext(buf) && (((uintptr_t)buf | len) & 63)) return -1;     /* PSRAM: whole cache lines only (gdma.h), else the
                                                                     cache sync would touch a neighbour's data */
    size_t max = ext(buf) ? GDMA_DESC_MAX & ~63u : GDMA_DESC_MAX;   /* PSRAM: whole 64-byte blocks per descriptor */
    do {
        if (i >= n) return -1;
        size_t k = len > max ? max : len;
        d[i].size = (k + 3) & ~3u;
        d[i].length = k;
        d[i].suc_eof = 0;
        d[i].err_eof = 0;
        d[i].owner = 1;                                    /* DMA owns it */
        d[i].buf = p;
        d[i].next = 0;
        if (i) d[i - 1].next = &d[i];
        p += k; len -= k; i++;
    } while (len);
    d[i - 1].suc_eof = eof;
    return i;
}

/* PSRAM buffers go through the data cache: before OUT the CPU's writes must reach PSRAM, before IN the cache must
 * hold no line of the buffer (no stale data after the DMA, no dirty line evicted over it). Rings end at their start. */
static void cache_sync(gdma_desc_t *d, bool in)
{
    gdma_desc_t *first = d;
    for (int n = 0; d && n < 256; n++) {
        if (ext(d->buf)) { psram_writeback(d->buf, d->size); if (in) psram_invalidate(d->buf, d->size); }
        d = d->next;
        if (d == first) break;
    }
}

void gdma_out_start(int c, gdma_desc_t *d)
{
    if (!(c >= 0 && c < 5)) return;      /* caller index: never past the hardware */
    cache_sync(d, false);
    REG(OUT_INT_CLR(c)) = ~0u;
    REG(OUT_LINK(c)) = ((uint32_t)d & 0xFFFFF);
    REG_SET(OUT_LINK(c), BIT(21));
}

void gdma_in_start(int c, gdma_desc_t *d)
{
    if (!(c >= 0 && c < 5)) return;      /* caller index: never past the hardware */
    cache_sync(d, true);
    REG(IN_INT_CLR(c)) = ~0u;
    REG(IN_LINK(c)) = ((uint32_t)d & 0xFFFFF);
    REG_SET(IN_LINK(c), BIT(22));
}

bool gdma_out_eof(int c) { return REG(OUT_INT_RAW(c)) & BIT(3); }
bool gdma_in_eof(int c)  { return REG(IN_INT_RAW(c)) & BIT(1); }

typedef struct { void (*fn)(void *); void *arg; } cb_t;
static cb_t out_cb[5], in_cb[5], m2m_cb[5];
static volatile uint8_t m2m_active;
static struct { void *dst; size_t len; } m2m_ext[5];        /* PSRAM destination to invalidate when done */                  /* bit per channel: async copy in flight */
static int8_t out_int[5] = { -1, -1, -1, -1, -1 }, in_int[5] = { -1, -1, -1, -1, -1 };

RAM_FN static void out_isr(void *a)
{
    int c = (int)a;
    uint32_t st = REG(OUT_INT_ST(c));
    REG(OUT_INT_CLR(c)) = st;
    if ((st & BIT(1)) && out_cb[c].fn) out_cb[c].fn(out_cb[c].arg);
}

RAM_FN static void in_isr(void *a)
{
    int c = (int)a;
    uint32_t st = REG(IN_INT_ST(c));
    REG(IN_INT_CLR(c)) = st;
    if ((st & (BIT(0) | BIT(1))) && in_cb[c].fn) in_cb[c].fn(in_cb[c].arg);
    if ((st & BIT(1)) && m2m_cb[c].fn) {                  /* async memcpy finished: one shot */
        cb_t cb = m2m_cb[c];
        m2m_cb[c].fn = 0;
        if (m2m_ext[c].dst) psram_invalidate(m2m_ext[c].dst, m2m_ext[c].len);
        m2m_active &= ~BIT(c);
        REG_CLR(IN_INT_ENA(c), BIT(1));
        cb.fn(cb.arg);
    }
}

int gdma_on_out_eof(int c, void (*fn)(void *), void *arg, int level)
{
    if (!(c >= 0 && c < 5)) return -1;      /* caller index: never past the hardware */
    REG_CLR(OUT_INT_ENA(c), BIT(1));
    out_cb[c] = (cb_t){ fn, arg };
    if (!fn) return 0;
    if (out_int[c] < 0 && (out_int[c] = irq_attach(SRC_OUT(c), level, out_isr, (void *)c)) < 0) return -1;
    REG(OUT_INT_CLR(c)) = BIT(1);
    REG_SET(OUT_INT_ENA(c), BIT(1));                      /* out_eof: each descriptor with eof */
    return 0;
}

int gdma_on_in_done(int c, void (*fn)(void *), void *arg, int level)
{
    if (!(c >= 0 && c < 5)) return -1;      /* caller index: never past the hardware */
    REG_CLR(IN_INT_ENA(c), BIT(0));
    in_cb[c] = (cb_t){ fn, arg };
    if (!fn) return 0;
    if (in_int[c] < 0 && (in_int[c] = irq_attach(SRC_IN(c), level, in_isr, (void *)c)) < 0) return -1;
    REG(IN_INT_CLR(c)) = BIT(0);
    REG_SET(IN_INT_ENA(c), BIT(0));                       /* in_done: each descriptor filled */
    return 0;
}

gdma_desc_t *gdma_out_eof_desc(int c) { return (gdma_desc_t *)REG(OUT_EOF_DES(c)); }
gdma_desc_t *gdma_in_eof_desc(int c)  { return (gdma_desc_t *)REG(IN_EOF_DES(c)); }

void gdma_stop(int c)
{
    REG_SET(OUT_LINK(c), BIT(20));
    REG_SET(IN_LINK(c), BIT(21));
}

/* per channel: 17 descriptors each way (4032 B each: whole 64-byte cache lines) -> a full 64 KB per transfer */
#define M2M_DESC 17
#define M2M_MAX (M2M_DESC * (GDMA_DESC_MAX & ~63u))
static gdma_desc_t m2m_out[5][M2M_DESC], m2m_in[5][M2M_DESC];

static int m2m_start(int c, void *dst, const void *src, size_t len)
{
    if (!(m2m_conn & BIT(c))) return -1;                  /* not gdma_connect(c, GDMA_M2M): it would drive a peripheral */
    if ((ext(dst) || ext(src)) && (((uint32_t)dst | (uint32_t)src | len) & 63)) return -1;   /* PSRAM: 64-byte lines */
    if (ext(src)) psram_writeback((void *)src, len);       /* the DMA reads PSRAM, not the cache */
    if (ext(dst)) psram_writeback(dst, len);               /* no dirty line may land on top of the DMA data later */
    m2m_ext[c].dst = ext(dst) ? dst : 0; m2m_ext[c].len = len;
    if (gdma_chain(m2m_in[c], M2M_DESC, dst, len, false) < 0) return -1;
    if (gdma_chain(m2m_out[c], M2M_DESC, (void *)src, len, true) < 0) return -1;
    gdma_in_start(c, m2m_in[c]);
    gdma_out_start(c, m2m_out[c]);
    return 0;
}

int gdma_memcpy(int c, void *dst, const void *src, size_t len)
{
    if (!(c >= 0 && c < 5)) return -1;      /* caller index: never past the hardware */
    uint8_t *d = dst; const uint8_t *s = src;
    while (len) {
        size_t k = len > M2M_MAX ? M2M_MAX : len;
        if (m2m_start(c, d, s, k) < 0) return -1;
        uint32_t t = ccount();
        while (!gdma_in_eof(c)) {                          /* ~20 MB/s floor: 1 ms per 16 KB */
            if (gdma_error(c)) { gdma_reset(c); return -3; }
            if (ccount() - t > 240000u * (1 + k / 16384)) { gdma_reset(c); return -2; }
        }
        if (m2m_ext[c].dst) psram_invalidate(d, k);
        d += k; s += k; len -= k;
    }
    return 0;
}

int gdma_memcpy_async(int c, void *dst, const void *src, size_t len, void (*done)(void *), void *arg, int level)
{
    if (!(c >= 0 && c < 5)) return -1;      /* caller index: never past the hardware */
    if (!len || len > M2M_MAX) return -1;
    if (done) {
        if (in_int[c] < 0 && (in_int[c] = irq_attach(SRC_IN(c), level, in_isr, (void *)c)) < 0) return -1;
        m2m_cb[c] = (cb_t){ done, arg };
        REG(IN_INT_CLR(c)) = BIT(1);
        REG_SET(IN_INT_ENA(c), BIT(1));
    }
    uint32_t ps = irq_lock(); m2m_active |= BIT(c); irq_unlock(ps);
    int r = m2m_start(c, dst, src, len);
    if (r < 0) { m2m_cb[c].fn = 0; ps = irq_lock(); m2m_active &= ~BIT(c); irq_unlock(ps); }
    return r;
}

bool gdma_busy(int c)
{
    if ((m2m_active & BIT(c)) && gdma_in_eof(c)) {
        uint32_t ps = irq_lock();
        if (m2m_active & BIT(c)) { m2m_active &= ~BIT(c); if (m2m_ext[c].dst && !m2m_cb[c].fn) psram_invalidate(m2m_ext[c].dst, m2m_ext[c].len); }
        irq_unlock(ps);
    }
    return m2m_active & BIT(c);
}
