/* P4 GDMA, same API as drivers/gdma/gdma.c (gdma.h). The P4 has two DMA controllers: AHB-DMA (3 channels: I2S, UHCI,
 * ADC, RMT, I3C) and AXI-DMA (3 channels: SPI2/3, LCD/CAM, AES, SHA, PARLIO), each channel an IN and an OUT half with
 * the S3's 12-byte linked-list descriptors. A peripheral is wired to one of them, so gdma_alloc() hands out a handle
 * (0..5) and gdma_connect() binds it to a free channel on the right controller; M2M goes on whichever has one.
 * Registers: ESP-IDF v6.1 hw_ver3 ahb_dma_reg.h / axi_dma_reg.h, esp_hal_dma/esp32p4 ahb_dma_ll.h / axi_dma_ll.h.
 * Cache: the L1 D-cache is in front of L2MEM, so starts write back (OUT) or write back + invalidate (IN) the buffers and
 * descriptors, and completion invalidates the IN buffers again (cache_p4.h). */
#include "gdma.h"
#include "clk.h"
#include "cpu.h"
#include "irq.h"
#include "soc.h"
#include "cache_p4.h"
#include "sys.h"

#define NH       6
enum { AHB = 0, AXI = 1 };

/* per-controller register map */
typedef struct {
    uint32_t base;
    uint32_t in_ch, out_ch, stride;          /* address of channel c's IN / OUT block: base + in_ch/out_ch + stride * c */
    uint32_t conf0, conf1, link, state, eof_des, pri, peri;   /* offsets inside the block (IN and OUT alike) */
    uint32_t intr_in, intr_out, intr_stride; /* raw / st / ena / clr at +0 / +4 / +8 / +0xC */
} ctl_t;
static const ctl_t ctl[2] = {
    { AHB_DMA_BASE, 0x70, 0x70 + 0x60, 0xC0, 0x00, 0x04, 0x10, 0x14, 0x18, 0x2C, 0x30, 0x00, 0x30, 0x10 },
    { AXI_DMA_BASE, 0x10, 0x138 + 0x10, 0x68, 0x00, 0x04, 0x10, 0x18, 0x1C, 0x30, 0x34, 0x00, 0x138, 0x68 },
};
/* Offsets are relative to a channel's IN / OUT block (AXI: from its conf registers, +0x10 into the channel).
 * IN link: stop b1, start b2; OUT link: stop b0, start b1. AHB keeps the descriptor address in a separate register
 * (below), AXI in LINK2 = link + 4. */
#define LINK_AHB_IN_ADDR(c)  (AHB_DMA_BASE + 0x3AC + 4 * (c))
#define LINK_AHB_OUT_ADDR(c) (AHB_DMA_BASE + 0x3B8 + 4 * (c))

typedef struct { int8_t bus, ch; } bind_t;
static bind_t bd[NH] = { { -1, -1 }, { -1, -1 }, { -1, -1 }, { -1, -1 }, { -1, -1 }, { -1, -1 } };
static uint8_t used;                         /* handles */
static uint8_t chused[2];                    /* physical channels per controller */
static bool m2m_h[NH];
static bool clk_up;

static volatile uint32_t gk;
static uint32_t glock(void) { uint32_t ps = irq_lock(); while (__atomic_exchange_n(&gk, 1, __ATOMIC_ACQUIRE)) {} return ps; }
static void gunlock(uint32_t ps) { __atomic_store_n(&gk, 0, __ATOMIC_RELEASE); irq_unlock(ps); }

#define BOUND(h) ((h) >= 0 && (h) < NH && bd[h].bus >= 0)
#define IN(h, o)  (ctl[bd[h].bus].base + ctl[bd[h].bus].in_ch  + ctl[bd[h].bus].stride * bd[h].ch + (o))
#define OUT(h, o) (ctl[bd[h].bus].base + ctl[bd[h].bus].out_ch + ctl[bd[h].bus].stride * bd[h].ch + (o))
#define IINT(h, o) (ctl[bd[h].bus].base + ctl[bd[h].bus].intr_in  + ctl[bd[h].bus].intr_stride * bd[h].ch + (o))
#define OINT(h, o) (ctl[bd[h].bus].base + ctl[bd[h].bus].intr_out + ctl[bd[h].bus].intr_stride * bd[h].ch + (o))
#define I_RAW 0x0
#define I_ST  0x4
#define I_ENA 0x8
#define I_CLR 0xC

static void hw_up(void)
{
    if (clk_up) return;
    REG_SET(HP_CLKRST_BASE + 0x18, BIT(3) | BIT(4));            /* SOC_CLK_CTRL1: AHB_PDMA, AXI_PDMA system clocks */
    REG_CLR(HP_CLKRST_BASE + 0xC4, BIT(1) | BIT(2));            /* HP_RST_EN1: AHB_PDMA, AXI_PDMA out of reset */
    REG_SET(AHB_DMA_BASE + 0x64, BIT(3));                        /* MISC_CONF.clk_en */
    REG(AHB_DMA_BASE + 0x404) = ~0u;                             /* MODULE_CLK_EN: all */
    REG(AHB_DMA_BASE + 0x3C4) = 0x40000000;                      /* intr_mem_start / end: internal range (ahb_dma_ll_set_default_memory_range) */
    REG(AHB_DMA_BASE + 0x3C8) = 0x4FFC0000;
    REG_SET(AXI_DMA_BASE + 0x2A8, BIT(4));                       /* MISC_CONF.clk_en */
    REG(AXI_DMA_BASE + 0x27C) = 0x4FC00000;                      /* axi_dma_ll_set_default_memory_range */
    REG(AXI_DMA_BASE + 0x280) = 0x4FFC0000;
    REG(AXI_DMA_BASE + 0x284) = 0x40000000;
    REG(AXI_DMA_BASE + 0x288) = 0x4C000000;
    clk_up = true;
}

int gdma_alloc(void)
{
    uint32_t ps = glock();
    int r = -1;
    for (int h = 0; h < NH && r < 0; h++) if (!(used & BIT(h))) { used |= BIT(h); r = h; }
    gunlock(ps);
    return r;
}

static void ch_reset(int h)
{
    REG_SET(IN(h, ctl[bd[h].bus].conf0), BIT(0));  REG_CLR(IN(h, ctl[bd[h].bus].conf0), BIT(0));
    REG_SET(OUT(h, ctl[bd[h].bus].conf0), BIT(0)); REG_CLR(OUT(h, ctl[bd[h].bus].conf0), BIT(0));
}

void gdma_stop(int h)
{
    if (!BOUND(h)) return;
    REG_SET(IN(h, 0x10), BIT(1));                                /* IN link: stop b1; OUT link: stop b0 (both controllers) */
    REG_SET(OUT(h, 0x10), BIT(0));
}

void gdma_free(int h)
{
    if (h < 0 || h >= NH) return;
    gdma_stop(h);
    uint32_t ps = glock();
    if (BOUND(h)) chused[bd[h].bus] &= ~BIT(bd[h].ch);
    bd[h] = (bind_t){ -1, -1 }; used &= ~BIT(h); m2m_h[h] = false;
    gunlock(ps);
}

/* P4 trigger ids: AHB I3C 0, UHCI0 2, I2S0 3, I2S1 4, I2S2 5, ADC 8, RMT 10; AXI LCD/CAM 0, SPI2 1, SPI3 2, PARLIO 3, AES 4, SHA 5 */
static bool periph_map(int periph, int *bus, int *id)
{
    switch (periph) {
    case GDMA_UHCI0: *bus = AHB; *id = 2; return true;
    case GDMA_I2S0:  *bus = AHB; *id = 3; return true;
    case GDMA_I2S1:  *bus = AHB; *id = 4; return true;
    case GDMA_ADC:   *bus = AHB; *id = 8; return true;
    case GDMA_RMT:   *bus = AHB; *id = 10; return true;
    case GDMA_SPI2:  *bus = AXI; *id = 1; return true;
    case GDMA_SPI3:  *bus = AXI; *id = 2; return true;
    case GDMA_LCD_CAM: *bus = AXI; *id = 0; return true;
    case GDMA_AES:   *bus = AXI; *id = 4; return true;
    case GDMA_SHA:   *bus = AXI; *id = 5; return true;
    default: return false;
    }
}

void gdma_connect(int h, int periph)
{
    if (h < 0 || h >= NH || !(used & BIT(h))) return;
    uint32_t ps = glock();
    hw_up();
    int bus = AHB, id = 0;
    bool m2m = periph == GDMA_M2M;
    if (!m2m && !periph_map(periph, &bus, &id)) { gunlock(ps); return; }
    if (BOUND(h)) { chused[bd[h].bus] &= ~BIT(bd[h].ch); bd[h] = (bind_t){ -1, -1 }; }
    int ch = -1;
    for (int pass = 0; pass < (m2m ? 2 : 1) && ch < 0; pass++) {
        int b = m2m ? pass : bus;
        for (int c = 0; c < 3 && ch < 0; c++) if (!(chused[b] & BIT(c))) { chused[b] |= BIT(c); ch = c; bus = b; }
    }
    if (ch < 0) { gunlock(ps); return; }
    bd[h] = (bind_t){ (int8_t)bus, (int8_t)ch };
    m2m_h[h] = m2m;
    if (m2m) { static const uint8_t ahb_free[3] = { 6, 7, 9 }; id = bus == AHB ? ahb_free[ch] : 6 + ch; }   /* an id no peripheral on this controller uses (IDF m2m_free_periph_mask) */
    gunlock(ps);
    const ctl_t *t = &ctl[bus];
    ch_reset(h);
    /* IN: mem_trans_en for M2M (AHB conf0 b4, AXI conf0 b2); OUT: EOF mode b3 (EOF when the peripheral popped the data) */
    REG(IN(h, t->conf0)) = m2m ? (bus == AHB ? BIT(4) : BIT(2)) : 0;
    REG(OUT(h, t->conf0)) = BIT(3);
    REG(IN(h, t->conf1)) = 0;
    REG(OUT(h, t->conf1)) = 0;
    REG(IN(h, t->peri)) = (uint32_t)id;
    REG(OUT(h, t->peri)) = (uint32_t)id;
}

void gdma_set_priority(int h, int prio)
{
    if (!BOUND(h)) return;
    REG(IN(h, ctl[bd[h].bus].pri)) = (uint32_t)prio; REG(OUT(h, ctl[bd[h].bus].pri)) = (uint32_t)prio;
}

void gdma_check_owner(int h, bool on)
{
    if (!BOUND(h)) return;
    if (on) { REG_SET(IN(h, ctl[bd[h].bus].conf1), BIT(12)); REG_SET(OUT(h, ctl[bd[h].bus].conf1), BIT(12)); }
    else    { REG_CLR(IN(h, ctl[bd[h].bus].conf1), BIT(12)); REG_CLR(OUT(h, ctl[bd[h].bus].conf1), BIT(12)); }
}

int gdma_error(int h)
{
    if (!BOUND(h)) return 0;
    return (REG(IINT(h, I_RAW)) & BIT(3) ? GDMA_ERR_IN : 0) | (REG(OINT(h, I_RAW)) & BIT(2) ? GDMA_ERR_OUT : 0);
}

void gdma_reset(int h)
{
    if (!BOUND(h)) return;
    gdma_stop(h);
    ch_reset(h);
    REG(IINT(h, I_CLR)) = ~0u; REG(OINT(h, I_CLR)) = ~0u;
}
void gdma_in_reset(int h)
{
    if (!BOUND(h)) return;
    REG_SET(IN(h, ctl[bd[h].bus].conf0), BIT(0)); REG_CLR(IN(h, ctl[bd[h].bus].conf0), BIT(0)); REG(IINT(h, I_CLR)) = ~0u;
}
void gdma_out_reset(int h)
{
    if (!BOUND(h)) return;
    REG_SET(OUT(h, ctl[bd[h].bus].conf0), BIT(0)); REG_CLR(OUT(h, ctl[bd[h].bus].conf0), BIT(0)); REG(OINT(h, I_CLR)) = ~0u;
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

/* write the chain (descriptors, and for OUT the data) back to memory; for IN drop the buffer lines as well */
static void cache_sync(gdma_desc_t *d, bool in)
{
    gdma_desc_t *first = d;
    for (int n = 0; d && n < 256; n++) {
        cache_dma_wbinv(d, sizeof *d);
        if (in) cache_dma_wbinv(d->buf, d->size); else cache_dma_writeback(d->buf, d->size);
        d = d->next;
        if (d == first) break;
    }
}

void gdma_out_start(int h, gdma_desc_t *d)
{
    if (!BOUND(h)) return;
    cache_sync(d, false);
    REG(OINT(h, I_CLR)) = ~0u;
    if (bd[h].bus == AHB) { REG(LINK_AHB_OUT_ADDR(bd[h].ch)) = (uint32_t)(uintptr_t)d; REG_SET(OUT(h, 0x10), BIT(1)); }
    else { REG(OUT(h, 0x14)) = (uint32_t)(uintptr_t)d; REG_SET(OUT(h, 0x10), BIT(1)); }
}

void gdma_in_start(int h, gdma_desc_t *d)
{
    if (!BOUND(h)) return;
    cache_sync(d, true);
    REG(IINT(h, I_CLR)) = ~0u;
    if (bd[h].bus == AHB) { REG(LINK_AHB_IN_ADDR(bd[h].ch)) = (uint32_t)(uintptr_t)d; REG_SET(IN(h, 0x10), BIT(2)); }
    else { REG(IN(h, 0x14)) = (uint32_t)(uintptr_t)d; REG_SET(IN(h, 0x10), BIT(2)); }
}

bool gdma_out_eof(int h) { return BOUND(h) && (REG(OINT(h, I_RAW)) & BIT(3)); }
bool gdma_in_eof(int h)  { return BOUND(h) && (REG(IINT(h, I_RAW)) & BIT(1)); }

typedef struct { void (*fn)(void *); void *arg; } cb_t;
static cb_t out_cb[NH], in_cb[NH], m2m_cb[NH];
static volatile uint8_t m2m_active;
static struct { void *dst; size_t len; } m2m_dst[NH];        /* destination to invalidate when done */
static int8_t out_int[NH] = { -1, -1, -1, -1, -1, -1 }, in_int[NH] = { -1, -1, -1, -1, -1, -1 };

RAM_FN static void out_isr(void *a)
{
    int h = (int)(uintptr_t)a;
    uint32_t st = REG(OINT(h, I_ST));
    REG(OINT(h, I_CLR)) = st;
    if ((st & BIT(1)) && out_cb[h].fn) out_cb[h].fn(out_cb[h].arg);
}

RAM_FN static void in_isr(void *a)
{
    int h = (int)(uintptr_t)a;
    uint32_t st = REG(IINT(h, I_ST));
    REG(IINT(h, I_CLR)) = st;
    if ((st & (BIT(0) | BIT(1))) && in_cb[h].fn) in_cb[h].fn(in_cb[h].arg);
    if ((st & BIT(1)) && m2m_cb[h].fn) {
        cb_t cb = m2m_cb[h];
        m2m_cb[h].fn = 0;
        if (m2m_dst[h].dst) cache_dma_wbinv(m2m_dst[h].dst, m2m_dst[h].len);
        m2m_active &= ~BIT(h);
        REG_CLR(IINT(h, I_ENA), BIT(1));
        cb.fn(cb.arg);
    }
}

static int src_in(int h) { return bd[h].bus == AHB ? SRC_AHB_PDMA_IN(bd[h].ch) : SRC_AXI_PDMA_IN(bd[h].ch); }
static int src_out(int h) { return bd[h].bus == AHB ? SRC_AHB_PDMA_OUT(bd[h].ch) : SRC_AXI_PDMA_OUT(bd[h].ch); }

int gdma_on_out_eof(int h, void (*fn)(void *), void *arg, int level)
{
    if (!BOUND(h)) return -1;
    REG_CLR(OINT(h, I_ENA), BIT(1));
    out_cb[h] = (cb_t){ fn, arg };
    if (!fn) return 0;
    if (out_int[h] < 0 && (out_int[h] = (int8_t)irq_attach(src_out(h), level, out_isr, (void *)(uintptr_t)h)) < 0) return -1;
    REG(OINT(h, I_CLR)) = BIT(1);
    REG_SET(OINT(h, I_ENA), BIT(1));
    return 0;
}

int gdma_on_in_done(int h, void (*fn)(void *), void *arg, int level)
{
    if (!BOUND(h)) return -1;
    REG_CLR(IINT(h, I_ENA), BIT(0));
    in_cb[h] = (cb_t){ fn, arg };
    if (!fn) return 0;
    if (in_int[h] < 0 && (in_int[h] = (int8_t)irq_attach(src_in(h), level, in_isr, (void *)(uintptr_t)h)) < 0) return -1;
    REG(IINT(h, I_CLR)) = BIT(0);
    REG_SET(IINT(h, I_ENA), BIT(0));
    return 0;
}

gdma_desc_t *gdma_out_eof_desc(int h) { return BOUND(h) ? (gdma_desc_t *)REG(OUT(h, ctl[bd[h].bus].eof_des)) : 0; }
gdma_desc_t *gdma_in_eof_desc(int h)  { return BOUND(h) ? (gdma_desc_t *)REG(IN(h, ctl[bd[h].bus].eof_des)) : 0; }

/* per handle: 17 descriptors each way (4032 B each) -> 64 KB per transfer, whole 64-byte lines */
#define M2M_DESC 17
#define M2M_MAX (M2M_DESC * (GDMA_DESC_MAX & ~63u))
static gdma_desc_t m2m_out[NH][M2M_DESC] __attribute__((aligned(64))), m2m_in[NH][M2M_DESC] __attribute__((aligned(64)));

static int m2m_start(int h, void *dst, const void *src, size_t len)
{
    if (!BOUND(h) || !m2m_h[h]) return -1;                 /* not gdma_connect(h, GDMA_M2M) */
    m2m_dst[h].dst = dst; m2m_dst[h].len = len;
    if (gdma_chain(m2m_in[h], M2M_DESC, dst, len, false) < 0) return -1;
    if (gdma_chain(m2m_out[h], M2M_DESC, (void *)src, len, true) < 0) return -1;
    gdma_in_start(h, m2m_in[h]);
    gdma_out_start(h, m2m_out[h]);
    return 0;
}

int gdma_memcpy(int h, void *dst, const void *src, size_t len)
{
    if (!BOUND(h)) return -1;
    uint8_t *d = dst; const uint8_t *s = src;
    uint32_t ms_cycles = sys_cpu_hz() / 1000;
    while (len) {
        size_t k = len > M2M_MAX ? M2M_MAX : len;
        if (m2m_start(h, d, s, k) < 0) return -1;
        uint32_t t = ccount();
        while (!gdma_in_eof(h)) {
            if (gdma_error(h)) { gdma_reset(h); return -3; }
            if (ccount() - t > ms_cycles * (1 + k / 16384)) { gdma_reset(h); return -2; }
        }
        cache_dma_wbinv(d, k);                              /* drop lines loaded while the DMA wrote */
        d += k; s += k; len -= k;
    }
    return 0;
}

int gdma_memcpy_async(int h, void *dst, const void *src, size_t len, void (*done)(void *), void *arg, int level)
{
    if (!BOUND(h) || !len || len > M2M_MAX) return -1;
    if (done) {
        if (in_int[h] < 0 && (in_int[h] = (int8_t)irq_attach(src_in(h), level, in_isr, (void *)(uintptr_t)h)) < 0) return -1;
        m2m_cb[h] = (cb_t){ done, arg };
        REG(IINT(h, I_CLR)) = BIT(1);
        REG_SET(IINT(h, I_ENA), BIT(1));
    }
    uint32_t ps = irq_lock(); m2m_active |= BIT(h); irq_unlock(ps);
    int r = m2m_start(h, dst, src, len);
    if (r < 0) { m2m_cb[h].fn = 0; ps = irq_lock(); m2m_active &= ~BIT(h); irq_unlock(ps); }
    return r;
}

bool gdma_busy(int h)
{
    if (h < 0 || h >= NH) return false;
    if ((m2m_active & BIT(h)) && gdma_in_eof(h)) {
        uint32_t ps = irq_lock();
        if (m2m_active & BIT(h)) { m2m_active &= ~BIT(h); if (!m2m_cb[h].fn) cache_dma_wbinv(m2m_dst[h].dst, m2m_dst[h].len); }
        irq_unlock(ps);
    }
    return m2m_active & BIT(h);
}
