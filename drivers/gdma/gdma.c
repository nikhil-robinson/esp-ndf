/* GDMA (TRM ch. 3). Channel register blocks are 0xC0 apart. */
#include "gdma.h"
#include "clk.h"
#include "xtensa.h"

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

void gdma_stop(int c)
{
    REG_SET(OUT_LINK(c), BIT(20));
    REG_SET(IN_LINK(c), BIT(21));
}

/* up to 16 descriptors -> 65 KB per call; larger copies loop */
static gdma_desc_t m2m_out[16], m2m_in[16];

int gdma_memcpy(int c, void *dst, const void *src, size_t len)
{
    uint8_t *d = dst; const uint8_t *s = src;
    while (len) {
        size_t k = len > 16 * GDMA_DESC_MAX ? 16 * GDMA_DESC_MAX : len;
        if (gdma_chain(m2m_in, 16, d, k, false) < 0) return -1;
        if (gdma_chain(m2m_out, 16, (void *)s, k, true) < 0) return -1;
        gdma_in_start(c, m2m_in);
        gdma_out_start(c, m2m_out);
        uint32_t t = ccount();
        while (!gdma_in_eof(c))                            /* ~20 MB/s floor: 1 ms per 16 KB */
            if (ccount() - t > 240000u * (1 + k / 16384)) { gdma_stop(c); return -2; }
        d += k; s += k; len -= k;
    }
    return 0;
}
