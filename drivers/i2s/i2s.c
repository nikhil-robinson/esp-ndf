/* I2S std mode (TRM ch. 28), HW v2: TDM engine with 2 slots = Philips stereo. */
#include "i2s.h"
#include "gpio.h"
#include "gdma.h"
#include "clk.h"
#include "xtensa.h"
#include "sys.h"
#include <string.h>

#define B(p)            ((p) ? 0x6002D000 : 0x6000F000)
#define RX_CONF(p)      (B(p) + 0x20)
#define TX_CONF(p)      (B(p) + 0x24)   /* b0 rst b1 fifo_rst b2 start b3 slave b8 update b12 pcm_bypass b15 left_align b19 tdm_en */
#define RX_CONF1(p)     (B(p) + 0x28)
#define TX_CONF1(p)     (B(p) + 0x2C)   /* b6:0 ws_width b12:7 bck_div b17:13 bits b23:18 half b28:24 chan_bits b29 msb_shift */
#define RX_CLKM(p)      (B(p) + 0x30)   /* b7:0 div b26 active b28:27 sel b29 mclk_sel */
#define TX_CLKM(p)      (B(p) + 0x34)   /* b7:0 div b26 active b28:27 sel b29 clk_en */
#define RX_CLKM_DIV(p)  (B(p) + 0x38)   /* b8:0 z b17:9 y b26:18 x b27 yn1 */
#define TX_CLKM_DIV(p)  (B(p) + 0x3C)
#define RX_TDM(p)       (B(p) + 0x50)   /* b15:0 chan en b19:16 tot_chan-1 */
#define TX_TDM(p)       (B(p) + 0x54)
#define PCM2PDM(p)      (B(p) + 0x40)   /* I2S0 only */
#define PCM2PDM1(p)     (B(p) + 0x44)
#define RXEOF_NUM(p)    (B(p) + 0x64)

#define SCLK  160000000u
#define NB    6                          /* blocks per ring */
#define BLK   1024                       /* bytes per block */

typedef struct {
    int ch;
    uint8_t txb[NB][BLK] __attribute__((aligned(4)));
    uint8_t rxb[NB][BLK] __attribute__((aligned(4)));
    gdma_desc_t txd[NB], rxd[NB];
    volatile uint32_t th, tt, rh, rt;    /* tx: app head / dma tail; rx: dma head / app tail */
    volatile uint32_t und, ovr;
    uint32_t tl, rl;                      /* last descriptor index each ISR handled */
    uint32_t woff;                        /* bytes already written into block th */
} port_t;
static port_t P[2] = { { .ch = -1 }, { .ch = -1 } };

static const uint8_t s_bck_o[2] = { 22, 28 }, s_ws_o[2] = { 24, 29 }, s_sd_o[2] = { 25, 30 }, s_mclk[2] = { 23, 21 };
static const uint8_t s_bck_i[2] = { 26, 31 }, s_ws_i[2] = { 27, 32 }, s_sd_i[2] = { 25, 30 };

/* best n/d (d <= 63) for the fractional part of SCLK / mclk */
static void frac(uint32_t mclk, uint32_t *ip, uint32_t *np, uint32_t *dp)
{
    uint32_t i = SCLK / mclk, rem = SCLK % mclk;
    uint64_t best = ~0ull; uint32_t bn = 0, bd = 1;
    for (uint32_t d = 1; d <= 63 && rem; d++) {
        uint32_t n = (uint32_t)(((uint64_t)rem * d + mclk / 2) / mclk);
        if (n >= d) continue;
        uint64_t err = (uint64_t)rem * d > (uint64_t)n * mclk ? (uint64_t)rem * d - (uint64_t)n * mclk
                                                             : (uint64_t)n * mclk - (uint64_t)rem * d;
        err = err * 64 / d;                               /* normalise across denominators */
        if (err < best) { best = err; bn = n; bd = d; }
    }
    *ip = i; *np = rem ? bn : 0; *dp = rem ? bd : 1;
}

/* Fractional MCLK divider N + n/d (TRM: x, y, z, yn1 encoding). The hardware
 * latches the fields in order: go through a safe small divider, then write
 * yn1, z, y, x one at a time, and the integer part last (as IDF does).
 * Writing the whole word at once leaves the fraction inactive (measured
 * with the logic analyzer: 48.077 kHz instead of 48.000 kHz). */
static void set_div(uint32_t clkm, uint32_t cdiv, uint32_t base, uint32_t i, uint32_t n, uint32_t d)
{
    uint32_t yn1 = 0, z = 0, x = 0, y = 0;
    if (n) { yn1 = n * 2 > d; z = yn1 ? d - n : n; x = d / z - 1; y = d % z; }
    REG(clkm) = base | 2;                                   /* small divider first */
    REG_FIELD(cdiv, 27, 1, 0);
    REG_FIELD(cdiv, 9, 9, 1);
    REG_FIELD(cdiv, 0, 9, 0);
    REG_FIELD(cdiv, 18, 9, 0);
    REG_FIELD(cdiv, 27, 1, yn1);                            /* target, in order */
    REG_FIELD(cdiv, 0, 9, z);
    REG_FIELD(cdiv, 9, 9, y);
    REG_FIELD(cdiv, 18, 9, x);
    REG(clkm) = base | i;
}

/* Both ISRs catch up to the descriptor the DMA last reported, so two EOFs merged into one
 * interrupt (late ISR at high data rates) cannot leave the ring a block behind the DMA. */
RAM_FN static void tx_done(void *a)                        /* blocks finished playing */
{
    port_t *p = a;
    uint32_t k = (uint32_t)(gdma_out_eof_desc(p->ch) - p->txd);
    if (k >= NB) return;
    while (p->tl != k) {
        p->tl = (p->tl + 1) % NB;
        uint32_t i = p->tt % NB;
        if (p->tt == p->th) p->und++;                       /* it was never filled: silence */
        for (int w = 0; w < BLK / 4; w++) ((uint32_t *)p->txb[i])[w] = 0;
        p->tt++;
        if (p->th < p->tt) { p->th = p->tt; p->woff = 0; }
    }
}

RAM_FN static void rx_done(void *a)                         /* blocks filled */
{
    port_t *p = a;
    uint32_t k = (uint32_t)(gdma_in_eof_desc(p->ch) - p->rxd);
    if (k >= NB) return;
    while (p->rl != k) {
        p->rl = (p->rl + 1) % NB;
        p->rh++;
        if (p->rh - p->rt > NB - 1) { p->rt++; p->ovr++; }  /* drop the oldest unread block */
    }
}

uint64_t i2s_init(int port, const i2s_cfg_t *c)
{
    uint32_t ns = c->mode == I2S_TDM && c->slots ? c->slots : 2, bits = c->bits;
    uint32_t mask = c->mask ? c->mask : (1u << ns) - 1;
    bool pdm = c->mode == I2S_PDM;
    bool duplex = c->dout >= 0 && c->din >= 0 && !c->slave;   /* IDF: RX slave on TX's BCK/WS (sig_loopback) */
    if (port < 0 || port > 1 || !c->rate || ns > 16 || ns * bits > 128 ||
        (bits != 8 && bits != 16 && bits != 24 && bits != 32) ||
        (pdm && (bits != 16 || port))) return 0;            /* PDM only on I2S0 */
    port_t *p = &P[port];
    periph_enable(port ? PERIPH_I2S1 : PERIPH_I2S0);
    /* BCK per mode (IDF i2s_std/tdm/pdm.c): frame clock; PDM TX rate*128 (fp/fs 960/480); PDM RX alone
     * rate*64 (DSR 8S), PDM duplex rate*128 with DSR 16S so RX runs on TX's clock */
    uint32_t bck = pdm ? c->rate * (c->dout >= 0 ? 128 : 64) : c->rate * ns * bits, bdiv = 8;
    while (bdiv > 2 && bck * bdiv > SCLK / 2) bdiv /= 2;           /* keep MCLK <= SCLK/2 */
    uint32_t i, n, d;
    frac(bck * bdiv, &i, &n, &d);
    if (pdm) { n = 0; d = 1; }                              /* IDF: integer MCLK divider in PDM (fraction = jitter: 5-8 dB SNR) */
    if (i < 2 || i > 255) return 0;
    uint32_t half = pdm ? 16 : ns * bits / 2;
    for (int dir = 0; dir < 2; dir++) {                      /* 0 = TX, 1 = RX */
        uint32_t clkm = dir ? RX_CLKM(port) : TX_CLKM(port), cdiv = dir ? RX_CLKM_DIV(port) : TX_CLKM_DIV(port);
        uint32_t conf = dir ? RX_CONF(port) : TX_CONF(port), conf1 = dir ? RX_CONF1(port) : TX_CONF1(port);
        uint32_t tdm = dir ? RX_TDM(port) : TX_TDM(port);
        set_div(clkm, cdiv, BIT(26) | 2u << 27 | BIT(29), i, n, d);
        REG_SET(conf, BIT(0)); REG_CLR(conf, BIT(0));
        REG_SET(conf, BIT(1)); REG_CLR(conf, BIT(1));
        uint32_t cb = pdm && !dir ? 32 : bits;               /* IDF: PDM TX stereo runs 32-bit slots */
        if (pdm)                                             /* as IDF leaves them: ws 0, RX msb_shift, TX bck_no_dly */
            REG(conf1) = (bdiv - 1) << 7 | (cb - 1) << 13 | (half - 1) << 18 | (cb - 1) << 24 | (dir ? BIT(29) : BIT(30));
        else {                                               /* framing as IDF's slot presets: WS width, 1-bit delay */
            uint32_t ws = c->fmt == I2S_PCM_SHORT ? 1 : c->fmt == I2S_PCM_LONG ? cb : half;
            REG(conf1) = (ws - 1) | (bdiv - 1) << 7 | (cb - 1) << 13 | (half - 1) << 18 | (cb - 1) << 24 |
                         (c->fmt != I2S_MSB ? BIT(29) : 0);
        }
        REG(conf) = (c->slave || (dir && duplex) ? BIT(3) : 0) | BIT(12) | BIT(15) |
                    (!pdm && c->fmt >= I2S_PCM_SHORT ? BIT(17) : 0) |   /* ws_idle_pol: PCM frames start on WS high */
                    (!pdm ? BIT(19) : dir ? BIT(20) | BIT(21) | (c->dout >= 0 ? BIT(22) : 0) : BIT(20)) |
                    (!dir && duplex ? BIT(27) : 0) |        /* tdm_en | pdm_en (+pdm2pcm, DSR16) | sig_loopback */
                    (pdm && !dir ? BIT(9) | BIT(13) : 0);
        REG(tdm) = pdm ? (dir ? 3 : 0xFFFF) : mask | (ns - 1) << 16;
    }
    if (pdm && port == 0) {
        /* IDF I2S_PDM_TX_SLOT_DEFAULT_CONFIG: hp on 35.5 Hz (6/7), osr2 2, hp /2, lp/sinc/sd x1, dither2, codec line */
        REG(PCM2PDM(port)) = 2u << 1 | 0u << 13 | 1u << 15 | 1u << 17 | 1u << 19 | BIT(21) | BIT(23) | BIT(25);
        REG(PCM2PDM1(port)) = 960 | 480u << 10 | 7u << 20 | 6u << 23;
    } else if (port == 0) REG_CLR(PCM2PDM(port), BIT(25));
    REG(RXEOF_NUM(port)) = BLK;                             /* in bytes: one EOF per block */
    if (p->ch < 0) {
        if ((p->ch = gdma_alloc()) < 0) return 0;
        gdma_connect(p->ch, port ? GDMA_I2S1 : GDMA_I2S0);
    }
    for (int k = 0; k < NB; k++) {                           /* circular rings, every block EOF */
        gdma_chain(&p->txd[k], 1, p->txb[k], BLK, true);
        gdma_chain(&p->rxd[k], 1, p->rxb[k], BLK, false);
        p->rxd[k].length = 0;
    }
    for (int k = 0; k < NB; k++) { p->txd[k].next = &p->txd[(k + 1) % NB]; p->rxd[k].next = &p->rxd[(k + 1) % NB]; }
    for (int k = 0; k < NB; k++) for (int w = 0; w < BLK / 4; w++) ((uint32_t *)p->txb[k])[w] = 0;
    p->th = p->tt = p->rh = p->rt = p->und = p->ovr = p->woff = 0;
    p->tl = p->rl = NB - 1;                                  /* the first EOF is block 0 */
    gdma_on_out_eof(p->ch, tx_done, p, 1);
    gdma_on_in_done(p->ch, rx_done, p, 1);

    if (c->slave) {
        if (c->bck >= 0) { gpio_config(c->bck, GPIO_INPUT | GPIO_FORCE); gpio_in_signal(c->bck, s_bck_i[port], false); }
        if (c->ws >= 0)  { if (pdm) gpio_config(c->ws, GPIO_INPUT | GPIO_FORCE); gpio_in_signal(c->ws, s_ws_i[port], false); }
    } else {
        if (c->bck >= 0) { gpio_config(c->bck, GPIO_OUTPUT | GPIO_INPUT); gpio_out_signal(c->bck, s_bck_o[port], false); gpio_in_signal(c->bck, s_bck_i[port], false); }
        if (c->ws >= 0)  { gpio_config(c->ws, GPIO_OUTPUT | GPIO_INPUT);  gpio_out_signal(c->ws, s_ws_o[port], false);  gpio_in_signal(c->ws, s_ws_i[port], false); }
        if (c->mclk >= 0){ gpio_config(c->mclk, GPIO_OUTPUT); gpio_out_signal(c->mclk, s_mclk[port], false); }
    }
    if (c->dout >= 0) { gpio_config(c->dout, GPIO_OUTPUT | GPIO_INPUT); gpio_out_signal(c->dout, s_sd_o[port], false); }
    if (c->din >= 0)  { gpio_config(c->din, GPIO_INPUT | GPIO_FORCE); gpio_in_signal(c->din, s_sd_i[port], false); }
    if (c->slave) return (uint64_t)c->rate * 1000;
    /* fs = SCLK / (i + n/d) / bdiv / (bck per sample) */
    return (uint64_t)SCLK * 1000 * d / ((uint64_t)(i * d + n) * bdiv * (bck / c->rate));
}

void i2s_start(int port, bool tx, bool rx)
{
    port_t *p = &P[port];
    gdma_reset(p->ch);                                      /* as IDF: else the EOF descriptor of the last run is reported */
    if (rx) {
        gdma_in_start(p->ch, &p->rxd[0]);
        REG_SET(RX_CONF(port), BIT(8)); while (REG(RX_CONF(port)) & BIT(8)) {}
        REG_SET(RX_CONF(port), BIT(2));
    }
    if (tx) {
        gdma_out_start(p->ch, &p->txd[0]);
        REG_SET(TX_CONF(port), BIT(8)); while (REG(TX_CONF(port)) & BIT(8)) {}
        REG_SET(TX_CONF(port), BIT(2));
    }
}

void i2s_stop(int port)
{
    REG_CLR(TX_CONF(port), BIT(2));
    REG_CLR(RX_CONF(port), BIT(2));
    gdma_stop(P[port].ch);
}

int i2s_write(int port, const void *buf, size_t len, uint32_t us)
{
    port_t *p = &P[port];
    const uint8_t *b = buf;
    uint32_t t0 = ccount(), lim = us * (sys_cpu_hz() / 1000000);
    size_t done = 0;
    while (done < len) {
        /* out EOF means "fetched", not "played": keep one block of margin behind the DMA */
        if (p->th - p->tt >= NB - 2) { if (ccount() - t0 > lim) break; continue; }
        uint32_t ps = irq_lock();
        uint8_t *blk = p->txb[(p->th + 1) % NB];
        size_t k = BLK - p->woff < len - done ? BLK - p->woff : len - done;
        memcpy(blk + p->woff, b + done, k);
        p->woff += k; done += k;
        if (p->woff == BLK) { p->th++; p->woff = 0; }
        irq_unlock(ps);
    }
    return done;
}

int i2s_read(int port, void *buf, size_t len, uint32_t us)
{
    port_t *p = &P[port];
    uint8_t *b = buf;
    uint32_t t0 = ccount(), lim = us * (sys_cpu_hz() / 1000000);
    size_t done = 0;
    while (done < len) {
        if (p->rh == p->rt) { if (ccount() - t0 > lim) break; continue; }
        uint32_t ps = irq_lock();
        uint8_t *blk = p->rxb[p->rt % NB];
        size_t have = p->rxd[p->rt % NB].length;              /* bytes the DMA actually wrote */
        size_t k = have < len - done ? have : len - done;     /* whole blocks; partial tail dropped */
        memcpy(b + done, blk, k);
        done += k; p->rt++;
        irq_unlock(ps);
    }
    return done;
}

uint32_t i2s_tx_underruns(int port) { return P[port].und; }
uint32_t i2s_rx_overruns(int port)  { return P[port].ovr; }
