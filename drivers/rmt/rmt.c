/* RMT driver. Channel RAM is accessed directly (APB_FIFO_MASK = 1). */
#include "rmt.h"
#include "gpio.h"
#include "clk.h"
#include "irq.h"
#include "xtensa.h"
#include "sys.h"
#include "gdma.h"

#define RMT_BASE        0x60016000
#define RMT_RAM(c)      ((volatile uint32_t *)(0x60016800 + (c) * 48 * 4))  /* c = 0..7 */
#define TX_CONF0(c)     (RMT_BASE + 0x20 + 4 * (c))
#define RX_CONF0(c)     (RMT_BASE + 0x30 + 8 * (c))   /* b7:0 div b22:8 idle_thres b27:24 mem_size */
#define RX_CONF1(c)     (RMT_BASE + 0x34 + 8 * (c))   /* b0 rx_en b1 wr_rst b3 owner b4 filt_en b12:5 thres b13 wrap b15 update */
#define RX_STATUS(c)    (RMT_BASE + 0x60 + 4 * (c))   /* b9:0 write address (absolute word index) */
#define INT_RAW         (RMT_BASE + 0x70)
#define INT_ST          (RMT_BASE + 0x74)
#define INT_ENA         (RMT_BASE + 0x78)
#define INT_CLR         (RMT_BASE + 0x7C)
#define CARRIER(c)      (RMT_BASE + 0x80 + 4 * (c))   /* b15:0 low b31:16 high (in group clock cycles) */
#define RX_CARRIER(c)   (RMT_BASE + 0x90 + 4 * (c))   /* b15:0 low-1 b31:16 high-1 (channel ticks) */
#define TX_LIM(c)       (RMT_BASE + 0xA0 + 4 * (c))   /* b8:0 threshold */
#define RX_LIM(c)       (RMT_BASE + 0xB0 + 4 * (c))
#define SYS_CONF        (RMT_BASE + 0xC0)
#define TX_SIM          (RMT_BASE + 0xC4)   /* b3:0 channels b4 enable */
#define REF_CNT_RST     (RMT_BASE + 0xC8)   /* b3:0 reset TX clock dividers */
#define SIG(c)          (81 + (c))
#define SRC_RMT         40

/* TX_CONF0 bits */
#define TX_START   BIT(0)
#define TX_RD_RST  BIT(1)
#define TX_MEMRST  BIT(2)
#define TX_WRAP    BIT(4)
#define TX_IDLE_LV BIT(5)
#define TX_IDLE_EN BIT(6)
#define TX_CARRIER BIT(21)
#define TX_UPDATE  BIT(24)
#define TX_CONTI   BIT(3)
#define TX_DMA     BIT(25)
/* TX_LIM: b18:9 loop count b19 loop count en b20 loop count reset b21 stop at count */
#define LOOP_END(c) BIT(12 + (c))

/* interrupt bits: TX c: end 0+c, thr 8+c; RX c: end 16+c, thr 24+c */
#define HALF 24

typedef struct { const uint32_t *src; size_t left; volatile bool busy; uint32_t base; bool dma; } tx_t;
typedef struct { uint32_t *dst; size_t max, got; volatile int done; uint32_t rd; } rx_t;
static tx_t tx[4];
static rx_t rx[4];
static int cpu_int = -1;

static void group_init(void)
{
    if (cpu_int >= 0) return;
    periph_enable(PERIPH_RMT);
    REG(SYS_CONF) = BIT(31) | BIT(26) | 1u << 24 | BIT(0);   /* clk on, APB source, /1, direct RAM */
    REG(INT_ENA) = 0;
    REG(INT_CLR) = ~0u;
    extern void rmt_isr(void *);
    cpu_int = irq_attach(SRC_RMT, 1, rmt_isr, 0);
}

/* copy up to `room` words of the pending TX data into channel RAM at word `at` */
static void tx_fill(int c, int at, int room)
{
    tx_t *t = &tx[c];
    volatile uint32_t *ram = RMT_RAM(c);
    int n = 0;
    while (n < room && t->left) { ram[at + n++] = *t->src++; t->left--; }
    if (n < room) ram[at + n] = 0;                          /* terminator */
}

static void rx_drain(int c, uint32_t upto)                 /* copy received words [rd, upto) */
{
    rx_t *r = &rx[c];
    volatile uint32_t *ram = RMT_RAM(4 + c);
    while (r->rd != upto) {
        uint32_t w = ram[r->rd];
        if (r->got < r->max) r->dst[r->got++] = w;
        r->rd = (r->rd + 1) % 48;
    }
}

RAM_FN void rmt_isr(void *arg)
{
    (void)arg;
    uint32_t st = REG(INT_ST);
    REG(INT_CLR) = st;
    for (int c = 0; c < 4; c++) {
        if (st & BIT(8 + c)) {                              /* TX: half consumed -> refill it */
            tx_t *t = &tx[c];
            tx_fill(c, t->base, HALF);
            t->base ^= HALF;
        }
        if (st & (BIT(0 + c) | LOOP_END(c))) {
            tx[c].busy = false;
            if (st & LOOP_END(c)) REG_CLR(INT_ENA, LOOP_END(c));
        }
        if (st & BIT(24 + c))                               /* RX: half full -> drain */
            rx_drain(c, (REG(RX_STATUS(c)) & 0x3FF) - (4 + c) * 48);
        if (st & BIT(16 + c)) {
            rx_drain(c, ((REG(RX_STATUS(c)) & 0x3FF) - (4 + c) * 48) % 48);
            REG_CLR(RX_CONF1(c), BIT(0));
            REG_SET(RX_CONF1(c), BIT(15));
            rx[c].done = 1;
        }
    }
}

uint32_t rmt_tx_init(int c, int pin, uint32_t hz, bool idle)
{
    if (c < 0 || c > 3 || !hz || 80000000 % hz) return 0;
    uint32_t div = 80000000 / hz;
    if (div < 1 || div > 255) return 0;
    group_init();
    REG(TX_CONF0(c)) = (div & 0xFF) << 8 | 1u << 16 | TX_IDLE_EN | (idle ? TX_IDLE_LV : 0) |
                       TX_RD_RST | TX_MEMRST | TX_UPDATE;
    REG(CARRIER(c)) = 0;
    if (gpio_config(pin, GPIO_OUTPUT | GPIO_INPUT) < 0) return 0;
    gpio_out_signal(pin, SIG(c), false);
    return hz;
}

int rmt_tx(int c, const rmt_item_t *items, size_t n)
{
    tx_t *t = &tx[c];
    if (t->busy) return -1;
    t->src = (const uint32_t *)items; t->left = n; t->busy = true; t->base = 0;
    bool wrap = n >= 48;                                     /* no room for the terminator */
    tx_fill(c, 0, 48);                                       /* whole RAM first */
    REG(TX_LIM(c)) = HALF;
    REG(INT_CLR) = BIT(c) | BIT(8 + c);
    REG_SET(INT_ENA, BIT(c) | (wrap ? BIT(8 + c) : 0));
    REG_CLR(INT_ENA, wrap ? 0 : BIT(8 + c));
    uint32_t cfg = REG(TX_CONF0(c)) & ~(TX_WRAP | TX_START);
    REG(TX_CONF0(c)) = cfg | (wrap ? TX_WRAP : 0) | TX_RD_RST | TX_UPDATE;
    REG(TX_CONF0(c)) = cfg | (wrap ? TX_WRAP : 0) | TX_START | TX_UPDATE;
    return 0;
}

void rmt_tx_wait(int c) { while (READ_ONCE(tx[c].busy)) {} }

void rmt_tx_carrier(int c, uint32_t hz, int duty)
{
    if (!hz) { REG_CLR(TX_CONF0(c), TX_CARRIER); return; }
    uint32_t per = 80000000 / hz, hi = per * duty / 100;
    REG(CARRIER(c)) = hi << 16 | (per - hi);
    REG_SET(TX_CONF0(c), TX_CARRIER | BIT(22) | TX_UPDATE);   /* modulate high level */
}

uint32_t rmt_rx_init(int c, int pin, uint32_t hz, uint32_t idle, uint32_t filter_ns)
{
    if (c < 0 || c > 3 || !hz || 80000000 % hz) return 0;
    uint32_t div = 80000000 / hz, filt = filter_ns * 80 / 1000;
    if (div < 1 || div > 255 || idle > 32767 || filt > 255) return 0;
    group_init();
    REG(RX_CONF0(c)) = div | idle << 8 | 1u << 24;               /* mem_size = 1 block (b27:24) */
    REG(RX_CONF1(c)) = (filt ? BIT(4) | filt << 5 : 0) | BIT(3) | BIT(1) | BIT(2) | BIT(15);
    REG(RX_LIM(c)) = HALF;
    gpio_config(pin, GPIO_INPUT | GPIO_FORCE);
    gpio_in_signal(pin, SIG(c), false);
    return hz;
}

int rmt_rx_start(int c, rmt_item_t *buf, size_t max)
{
    rx[c] = (rx_t){ .dst = (uint32_t *)buf, .max = max };
    REG_CLR(RX_CONF0(c), BIT(23));                          /* back from DMA mode */
    REG(RX_LIM(c)) = HALF;
    REG(INT_CLR) = BIT(16 + c) | BIT(24 + c);
    REG_SET(INT_ENA, BIT(16 + c) | BIT(24 + c));
    uint32_t c1 = REG(RX_CONF1(c)) & ~BIT(0);
    REG(RX_CONF1(c)) = c1 | BIT(1) | BIT(2) | BIT(13) | BIT(15);        /* reset write ptr, wrap on */
    REG(RX_CONF1(c)) = (c1 & ~(BIT(1) | BIT(2))) | BIT(13) | BIT(0) | BIT(3) | BIT(15);
    return 0;
}

int rmt_rx_wait(int c, uint32_t us)
{
    uint32_t t = ccount(), lim = us * (sys_cpu_hz() / 1000000);
    while (!READ_ONCE(rx[c].done)) if (ccount() - t > lim) return -1;
    return rx[c].got;
}

/* ---- loop, sync, demodulation, DMA, WS2812 ---- */

int rmt_tx_loop(int c, const rmt_item_t *items, size_t n, uint32_t loops)
{
    tx_t *t = &tx[c];
    if (c < 0 || c > 3 || t->busy || !n || n > 47 || loops > 1023) return -1;
    volatile uint32_t *ram = RMT_RAM(c);
    for (size_t i = 0; i < n; i++) ram[i] = items[i].val;
    ram[n] = 0;                                             /* end of one pass */
    t->left = 0; t->busy = true;
    /* IDF rmt_tx: conti mode, loop count + auto stop, LOOP_END interrupt; loops = 0: until rmt_tx_stop */
    REG(TX_LIM(c)) = HALF | BIT(20);
    REG(TX_LIM(c)) = HALF | (loops ? loops << 9 | BIT(19) | BIT(21) : 0);
    REG(INT_CLR) = BIT(c) | LOOP_END(c);
    REG_CLR(INT_ENA, BIT(8 + c) | BIT(c));
    if (loops) REG_SET(INT_ENA, LOOP_END(c));
    uint32_t cfg = REG(TX_CONF0(c)) & ~(TX_WRAP | TX_START);
    REG(TX_CONF0(c)) = cfg | TX_CONTI | TX_RD_RST | TX_UPDATE;
    REG(TX_CONF0(c)) = cfg | TX_CONTI | TX_START | TX_UPDATE;
    return 0;
}

void rmt_tx_stop(int c)
{
    REG(TX_CONF0(c)) = (REG(TX_CONF0(c)) & ~(TX_CONTI | TX_START)) | BIT(7) | TX_UPDATE;   /* tx_stop */
    REG(TX_LIM(c)) = HALF;
    REG_CLR(INT_ENA, LOOP_END(c));
    tx[c].busy = false;
}

void rmt_tx_sync(uint32_t mask)
{
    /* IDF sync manager: channels in TX_SIM start together once all of them got start; reset their dividers */
    REG(TX_SIM) = mask ? (mask & 15) | BIT(4) : 0;
    if (mask) REG(REF_CNT_RST) = mask & 15;
}

void rmt_rx_demod(int c, uint32_t hz, uint32_t res_hz, int duty_pct, bool active_high)
{
    if (!hz) { REG_CLR(RX_CONF0(c), BIT(28)); return; }
    /* IDF rmt_rx_demodulate_carrier: thresholds in channel ticks */
    /* the thresholds are maxima: +25 % so a carrier at the nominal duty still counts */
    uint32_t tot = res_hz / hz, hi = tot * duty_pct * 5 / 400, lo = (tot - tot * duty_pct / 100) * 5 / 4;
    if (!hi) hi = 1;
    if (!lo) lo = 1;
    REG(RX_CARRIER(c)) = (hi - 1) << 16 | (lo - 1);
    REG(RX_CONF0(c)) = (REG(RX_CONF0(c)) & ~BIT(29)) | BIT(28) | (active_high ? BIT(29) : 0);
    REG_SET(RX_CONF1(c), BIT(15));
}

/* DMA: TX channel 3 and RX channel 3 only (TRM: RMT ch3 / ch7). Buffers in internal RAM. */
static int dma_ch = -1;
static gdma_desc_t dtx[9], drx[8];
static const uint32_t s_zero __attribute__((aligned(4))) = 0;

static int dma_get(void)
{
    if (dma_ch < 0 && (dma_ch = gdma_alloc()) >= 0) gdma_connect(dma_ch, GDMA_RMT);
    return dma_ch;
}

int rmt_tx_dma(int c, const rmt_item_t *items, size_t n)
{
    tx_t *t = &tx[c];
    if (c != 3 || t->busy || dma_get() < 0) return -1;
    int k = gdma_chain(dtx, 8, (void *)items, n * 4, false);
    if (k < 0) return -1;
    gdma_chain(&dtx[k], 1, (void *)&s_zero, 4, true);       /* terminator ends the frame */
    dtx[k - 1].next = &dtx[k];
    t->busy = true; t->left = 0; t->dma = true;
    REG(INT_CLR) = BIT(c);
    REG_CLR(INT_ENA, BIT(8 + c));
    REG_SET(INT_ENA, BIT(c));
    uint32_t cfg = REG(TX_CONF0(c)) & ~(TX_WRAP | TX_START | TX_CONTI);
    /* IDF: DMA streams through the channel RAM as a ring (wrap), prefill ~1 us before start */
    REG(TX_CONF0(c)) = cfg | TX_DMA | TX_WRAP | TX_RD_RST | TX_MEMRST | BIT(23) | TX_UPDATE;   /* + afifo reset */
    gdma_out_start(dma_ch, dtx);
    delay_us(1);
    REG(TX_CONF0(c)) = cfg | TX_DMA | TX_WRAP | TX_START | TX_UPDATE;
    return 0;
}

int rmt_rx_dma_start(int c, rmt_item_t *buf, size_t max)
{
    if (c != 3 || dma_get() < 0) return -1;
    if (gdma_chain(drx, 8, buf, max * 4, false) < 0) return -1;
    rx[c] = (rx_t){ .dst = (uint32_t *)buf, .max = max };
    REG(INT_CLR) = BIT(16 + c) | BIT(24 + c);
    REG_CLR(INT_ENA, BIT(24 + c));
    REG_SET(INT_ENA, BIT(16 + c));
    REG_SET(RX_CONF0(c), BIT(23));                          /* dma_access_en */
    REG(RX_LIM(c)) = 0;                                     /* IDF: no ping-pong limit with DMA */
    uint32_t c1 = REG(RX_CONF1(c)) & ~BIT(0);
    REG(RX_CONF1(c)) = c1 | BIT(1) | BIT(2) | BIT(13) | BIT(15);
    gdma_in_start(dma_ch, drx);
    REG(RX_CONF1(c)) = (c1 & ~(BIT(1) | BIT(2))) | BIT(0) | BIT(3) | BIT(13) | BIT(15);   /* IDF: wrap always on */
    rx[c].got = ~0u;                                        /* counted from the descriptors at the end */
    return 0;
}

int rmt_rx_dma_wait(int c, uint32_t us)
{
    uint32_t t = ccount(), lim = us * (sys_cpu_hz() / 1000000);
    while (!READ_ONCE(rx[c].done)) if (ccount() - t > lim) return -1;
    size_t bytes = 0;
    for (int i = 0; i < 8; i++) { bytes += drx[i].length; if (!drx[i].next || drx[i].suc_eof) break; }
    return bytes / 4;
}

size_t rmt_ws2812_encode(const uint8_t *grb, size_t nbytes, rmt_item_t *out, uint32_t res_hz)
{
    /* WS2812B: 0 = 0.4 us high / 0.85 us low, 1 = 0.8 / 0.45; >= 50 us low resets (held by idle level) */
    uint32_t t04 = res_hz / 2500000, t08 = res_hz / 1250000, t085 = res_hz * 17 / 20000000, t045 = res_hz * 9 / 20000000;
    size_t k = 0;
    for (size_t i = 0; i < nbytes; i++)
        for (int b = 7; b >= 0; b--)
            out[k++] = (grb[i] >> b) & 1 ? RMT_ITEM(1, t08, 0, t045) : RMT_ITEM(1, t04, 0, t085);
    return k;
}
