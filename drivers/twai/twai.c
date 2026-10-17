/* TWAI driver. Register map = SJA1000 PeliCAN, one register per 32-bit word. */
#include "twai.h"
#include "gpio.h"
#include "clk.h"
#include "irq.h"
#include "xtensa.h"
#include "sys.h"

#define T_BASE   0x6002B000
#define MOD      (T_BASE + 0x00)   /* b0 reset b1 listen b2 self-test b3 single filter */
#define CMR      (T_BASE + 0x04)   /* b0 tx req b1 abort b2 release rx b3 clr overrun b4 self rx req */
#define SR       (T_BASE + 0x08)   /* b0 rx buf b2 tx buf free b3 tx complete b7 bus off */
#define IR       (T_BASE + 0x0C)   /* read clears; b0 rx b3 overrun */
#define IER      (T_BASE + 0x10)
#define BTR0     (T_BASE + 0x18)   /* b12:0 brp b15:14 sjw */
#define BTR1     (T_BASE + 0x1C)   /* b3:0 tseg1 b6:4 tseg2 b7 sam */
#define RXERR    (T_BASE + 0x38)
#define TXERR    (T_BASE + 0x3C)
#define BUF(i)   (T_BASE + 0x40 + 4 * (i))   /* frame buffer / ACR0-3, AMR0-3 in reset mode */
#define CDR      (T_BASE + 0x7C)
#define SIG_TX   116
#define SIG_RX   116
#define SRC_TWAI 37

#define RING 16
static twai_frame_t ring[RING];
static volatile uint32_t rh, rt, dropped;
static twai_mode_t mode_;
static bool irq_on;

static void reset_mode(bool on)
{
    if (on) { REG_SET(MOD, BIT(0)); while (!(REG(MOD) & BIT(0))) {} }
    else    { REG_CLR(MOD, BIT(0)); while (REG(MOD) & BIT(0)) {} }
}

RAM_FN static void twai_isr(void *a)
{
    (void)a;
    uint32_t ir = REG(IR);                                  /* reading clears */
    if (ir & BIT(3)) REG(CMR) = BIT(3);                     /* overrun: clear */
    while (REG(SR) & BIT(0)) {                              /* drain the 64-byte RX FIFO */
        uint32_t info = REG(BUF(0));
        twai_frame_t f = { .ext = info >> 7 & 1, .rtr = info >> 6 & 1, .dlc = info & 15 };
        int d;
        if (f.ext) {
            f.id = REG(BUF(1)) << 21 | REG(BUF(2)) << 13 | REG(BUF(3)) << 5 | REG(BUF(4)) >> 3;
            d = 5;
        } else {
            f.id = REG(BUF(1)) << 3 | REG(BUF(2)) >> 5;
            d = 3;
        }
        if (f.dlc > 8) f.dlc = 8;
        for (int i = 0; i < f.dlc && !f.rtr; i++) f.data[i] = REG(BUF(d + i));
        REG(CMR) = BIT(2);                                  /* release the buffer */
        if (rh - rt < RING) ring[rh++ % RING] = f;
        else dropped++;
    }
}

int twai_init(int tx, int rx, uint32_t bitrate, twai_mode_t mode)
{
    /* 20 tq per bit: tseg1 15, tseg2 4 (80 % sample point); tq = brp / 80 MHz */
    if (!bitrate || 80000000 % (bitrate * 20)) return -1;
    uint32_t brp = 80000000 / (bitrate * 20);
    if (brp < 2 || brp > 16384 || (brp & 1)) return -1;
    periph_enable(PERIPH_TWAI);
    reset_mode(true);
    REG(CDR) = BIT(7);                                      /* PeliCAN mode, CLKOUT off */
    REG(BTR0) = (brp / 2 - 1) | (3 - 1) << 14;              /* sjw 3 */
    REG(BTR1) = (15 - 1) | (4 - 1) << 4;
    REG(MOD) = BIT(0) | BIT(3) | (mode == TWAI_SELFTEST ? BIT(2) : 0) | (mode == TWAI_LISTEN ? BIT(1) : 0);
    for (int i = 0; i < 4; i++) { REG(BUF(i)) = 0; REG(BUF(4 + i)) = 0xFF; }   /* accept everything */
    REG(IER) = BIT(0) | BIT(3);
    mode_ = mode;
    rh = rt = dropped = 0;
    gpio_config(tx, GPIO_OUTPUT | GPIO_INPUT); gpio_set(tx);
    gpio_out_signal(tx, SIG_TX, false);
    gpio_config(rx, GPIO_INPUT | GPIO_PULLUP | GPIO_FORCE);
    gpio_in_signal(rx, SIG_RX, false);
    if (!irq_on) irq_on = irq_attach(SRC_TWAI, 1, twai_isr, 0) >= 0;
    (void)REG(IR);
    reset_mode(false);
    return 0;
}

void twai_filter(uint32_t code, uint32_t mask, bool ext)
{
    /* single filter: ACR/AMR hold the ID left-aligned; AMR bit 1 = don't care */
    uint32_t c = ext ? code << 3 : code << 21, m = ext ? ~(mask << 3) : ~(mask << 21);
    if (!ext) m |= 0x1FFFFF;                                /* ignore RTR + data bytes */
    reset_mode(true);
    for (int i = 0; i < 4; i++) { REG(BUF(i)) = c >> (24 - 8 * i) & 0xFF; REG(BUF(4 + i)) = m >> (24 - 8 * i) & 0xFF; }
    reset_mode(false);
}

int twai_send(const twai_frame_t *f, uint32_t us)
{
    uint32_t t0 = ccount(), lim = us * (sys_cpu_hz() / 1000000);
    while (!(REG(SR) & BIT(2))) if (ccount() - t0 > lim) return -1;   /* tx buffer free */
    uint8_t dlc = f->dlc > 8 ? 8 : f->dlc;
    REG(BUF(0)) = (f->ext ? BIT(7) : 0) | (f->rtr ? BIT(6) : 0) | dlc;
    int d;
    if (f->ext) {
        uint32_t id = f->id << 3;
        REG(BUF(1)) = id >> 24; REG(BUF(2)) = id >> 16 & 0xFF; REG(BUF(3)) = id >> 8 & 0xFF; REG(BUF(4)) = id & 0xF8;
        d = 5;
    } else {
        REG(BUF(1)) = f->id >> 3 & 0xFF; REG(BUF(2)) = (f->id & 7) << 5;
        d = 3;
    }
    for (int i = 0; i < dlc && !f->rtr; i++) REG(BUF(d + i)) = f->data[i];
    REG(CMR) = mode_ == TWAI_SELFTEST ? BIT(4) : BIT(0);    /* self-reception in self test */
    while (!(REG(SR) & BIT(3))) if (ccount() - t0 > lim) { REG(CMR) = BIT(1); return -1; }
    return 0;
}

int twai_recv(twai_frame_t *f, uint32_t us)
{
    uint32_t t0 = ccount(), lim = us * (sys_cpu_hz() / 1000000);
    while (rh == rt) if (ccount() - t0 > lim) return -1;
    *f = ring[rt++ % RING];
    return 0;
}

void twai_counters(uint8_t *tx, uint8_t *rx, uint32_t *drop)
{
    if (tx) *tx = REG(TXERR);
    if (rx) *rx = REG(RXERR);
    if (drop) *drop = dropped;
}

void twai_recover(void)
{
    /* bus-off sets reset mode; leaving it starts the 128 x 11 recessive-bit recovery */
    reset_mode(false);
}
