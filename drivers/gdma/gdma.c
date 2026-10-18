/* GDMA (TRM ch. 3). Channel register blocks are 0xC0 apart. */
#include "gdma.h"
#include "clk.h"
#include "xtensa.h"
#include "irq.h"

#define G_BASE           0x6003F000
#define IN_CONF0(c)      (G_BASE + 0xC0 * (c) + 0x00)   /* b0 rst b2/b3 burst b4 mem_trans */
#define IN_CONF1(c)      (G_BASE + 0xC0 * (c) + 0x04)   /* b12 check owner */
#define IN_INT_RAW(c)    (G_BASE + 0xC0 * (c) + 0x08)   /* b0 done b1 suc_eof b3 dscr_err */
#define IN_INT_CLR(c)    (G_BASE + 0xC0 * (c) + 0x14)
#define IN_LINK(c)       (G_BASE + 0xC0 * (c) + 0x20)   /* b19:0 addr b20 auto_ret b21 stop b22 start */
#define IN_PERI(c)       (G_BASE + 0xC0 * (c) + 0x48)
#define OUT_CONF0(c)     (G_BASE + 0xC0 * (c) + 0x60)   /* b0 rst b2 auto_wrback b3 eof_mode b4/b5 burst */
#define OUT_CONF1(c)     (G_BASE + 0xC0 * (c) + 0x64)
#define OUT_INT_RAW(c)   (G_BASE + 0xC0 * (c) + 0x68)   /* b0 done b1 eof b3 total_eof */
#define OUT_INT_CLR(c)   (G_BASE + 0xC0 * (c) + 0x74)
#define OUT_LINK(c)      (G_BASE + 0xC0 * (c) + 0x80)   /* b19:0 addr b20 stop b21 start */
#define OUT_PERI(c)      (G_BASE + 0xC0 * (c) + 0xA8)
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

int gdma_alloc(void)
{
    if (!used) {
        periph_enable(PERIPH_GDMA);
        REG_SET(MISC_CONF, BIT(4));
        REG_SET(MISC_CONF, BIT(0)); REG_CLR(MISC_CONF, BIT(0));
    }
    for (int c = 0; c < 5; c++) if (!(used & BIT(c))) { used |= BIT(c); return c; }
    return -1;
}

void gdma_free(int c) { gdma_stop(c); used &= ~BIT(c); }

void gdma_connect(int c, int periph)
{
    REG_SET(IN_CONF0(c), BIT(0));  REG_CLR(IN_CONF0(c), BIT(0));
    REG_SET(OUT_CONF0(c), BIT(0)); REG_CLR(OUT_CONF0(c), BIT(0));
    REG(IN_CONF0(c)) = BIT(2) | BIT(3) | (periph == GDMA_M2M ? BIT(4) : 0);
    REG(OUT_CONF0(c)) = BIT(3) | BIT(4) | BIT(5);      /* EOF when data popped by peripheral */
    REG(IN_CONF1(c)) = 0;
    REG(OUT_CONF1(c)) = 0;
    /* M2M must still use a valid peripheral id (0..9: PMS grants SRAM access
     * per peripheral; 10+ is refused with a descriptor error). AES (6) is
     * unused by esp-ndf, so memory-to-memory borrows it. */
    int sel = periph == GDMA_M2M ? GDMA_AES : periph;
    REG(IN_PERI(c)) = sel;
    REG(OUT_PERI(c)) = sel;
}

int gdma_chain(gdma_desc_t *d, int n, void *buf, size_t len, bool eof)
{
    uint8_t *p = buf;
    int i = 0;
    do {
        if (i >= n) return -1;
        size_t k = len > GDMA_DESC_MAX ? GDMA_DESC_MAX : len;
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

void gdma_out_start(int c, gdma_desc_t *d)
{
    REG(OUT_INT_CLR(c)) = ~0u;
    REG(OUT_LINK(c)) = ((uint32_t)d & 0xFFFFF);
    REG_SET(OUT_LINK(c), BIT(21));
}

void gdma_in_start(int c, gdma_desc_t *d)
{
    REG(IN_INT_CLR(c)) = ~0u;
    REG(IN_LINK(c)) = ((uint32_t)d & 0xFFFFF);
    REG_SET(IN_LINK(c), BIT(22));
}

bool gdma_out_eof(int c) { return REG(OUT_INT_RAW(c)) & BIT(3); }
bool gdma_in_eof(int c)  { return REG(IN_INT_RAW(c)) & BIT(1); }

typedef struct { void (*fn)(void *); void *arg; } cb_t;
static cb_t out_cb[5], in_cb[5], m2m_cb[5];
static volatile uint8_t m2m_active;                  /* bit per channel: async copy in flight */
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
        m2m_active &= ~BIT(c);
        REG_CLR(IN_INT_ENA(c), BIT(1));
        cb.fn(cb.arg);
    }
}

int gdma_on_out_eof(int c, void (*fn)(void *), void *arg, int level)
{
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

/* per channel: 16 descriptors each way -> 65 KB per transfer */
#define M2M_MAX (16 * GDMA_DESC_MAX)
static gdma_desc_t m2m_out[5][16], m2m_in[5][16];

static int m2m_start(int c, void *dst, const void *src, size_t len)
{
    if (gdma_chain(m2m_in[c], 16, dst, len, false) < 0) return -1;
    if (gdma_chain(m2m_out[c], 16, (void *)src, len, true) < 0) return -1;
    gdma_in_start(c, m2m_in[c]);
    gdma_out_start(c, m2m_out[c]);
    return 0;
}

int gdma_memcpy(int c, void *dst, const void *src, size_t len)
{
    uint8_t *d = dst; const uint8_t *s = src;
    while (len) {
        size_t k = len > M2M_MAX ? M2M_MAX : len;
        if (m2m_start(c, d, s, k) < 0) return -1;
        uint32_t t = ccount();
        while (!gdma_in_eof(c))                            /* ~20 MB/s floor: 1 ms per 16 KB */
            if (ccount() - t > 240000u * (1 + k / 16384)) { gdma_stop(c); return -2; }
        d += k; s += k; len -= k;
    }
    return 0;
}

int gdma_memcpy_async(int c, void *dst, const void *src, size_t len, void (*done)(void *), void *arg, int level)
{
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
    if ((m2m_active & BIT(c)) && gdma_in_eof(c)) { uint32_t ps = irq_lock(); m2m_active &= ~BIT(c); irq_unlock(ps); }
    return m2m_active & BIT(c);
}
