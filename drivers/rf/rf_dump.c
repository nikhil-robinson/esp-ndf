#include "rf_dump.h"

#define REG(a) (*(volatile uint32_t *)(a))
#define CTRL     0x60033D5Cu                    /* 31 run, 19 start, 18 done, 16/15 rate, 13:0 count */
#define CONFIG   0x60033D90u
#define DAC      0x60033D64u                    /* 31 run, 19 trigger/loop, 18 done, 15 80 MS/s (else 40), 13:0 count */
#define BANK_SEL 0x600C101Cu                    /* 3:0 one-hot banks the engine owns */
#define UNWRITTEN 0xA5A0055Au                   /* bits 31:28 never written by the engine */

void rf_dac_play(unsigned bank, uint32_t n, int rate80, int loop)
{
    uint32_t ctrl = 1u << 31 | (rate80 ? 1u << 15 : 0) | n;
    REG(BANK_SEL) = (REG(BANK_SEL) & ~15u) | 1u << bank;
    REG(DAC) = 0;
    REG(DAC) = ctrl;
    REG(DAC) = ctrl | 1u << 19;                 /* held: loops gapless; released: one pass */
    if (!loop) REG(DAC) = ctrl;
}

void rf_dac_stop(void)
{
    REG(DAC) = 0;
    REG(BANK_SEL) &= ~15u;
}

/* linked in only together with the dump engine: the linker script checks its size (SRAM banks the engine writes) */
const uint8_t rf_dump_ram_marker __attribute__((section(".rfmark"), used)) = 1;
int rf_dump_burst(uint32_t rate, uint32_t n)
{
    __asm__ volatile("" :: "r"(&rf_dump_ram_marker));          /* reference: kept exactly when this function is */
    volatile uint32_t *w = (volatile uint32_t *)RF_DUMP_BURST_ADDR;
    if (n > RF_DUMP_MAX) n = RF_DUMP_MAX;
    for (uint32_t i = 0; i < n; i++) w[i] = UNWRITTEN;
    uint32_t owner = REG(BANK_SEL), ctrl = 1u << 31 | rate | n;
    REG(CTRL) = 0;
    REG(CONFIG) = 0x000C2040u;
    REG(BANK_SEL) = owner | 4u;                 /* keep a playing DAC's bank (shared counter: n = DAC count) */
    REG(CTRL) = ctrl;
    REG(CTRL) = ctrl | 1u << 19;
    REG(CTRL) = ctrl;
    uint32_t spin = 0;
    while (!(REG(CTRL) & 1u << 18) && ++spin < 10000000u) {}
    REG(CTRL) = 0;
    REG(BANK_SEL) = owner;
    return w[n - 1] == UNWRITTEN ? -1 : 0;
}

/* Gapless TX streaming. The DAC loops a 16384-word ring; the CPU cannot write the bank the DAC reads (measured),
 * so banks 0 and 1 alternate: unit k (8192 words = 205 us at 40 MS/s) plays from bank k%2, ring half k%2. While
 * unit k plays, unit k+1 goes into the other bank together with a copy of unit k's last TXTAIL words, so the bank
 * switch may happen anywhere in that tail. The DAC position comes from the CPU clock: the DAC's 40 MS/s and the
 * 240 MHz CPU share the crystal (measured exact), so position = cycles / 6. (A parallel capture as position
 * sensor stops the DAC: measured.) The tail is kept in RAM: reading the bank the DAC plays disturbs it too. */
static inline uint32_t ccount_now(void) { uint32_t c; __asm__ volatile("rsr.ccount %0" : "=r"(c)); return c; }
#define TXU      8192u
#define TXTAIL   2048u                                 /* switch window 51 us; the copy costs ~34 us */
#define CPS      6u                                    /* CPU cycles per DAC sample (240 MHz / 40 MS/s) */
int rf_dac_stream(uint32_t units, rf_dac_cb cb, void *ctx, rf_dac_stats_t *st)
{
    uint32_t *bank[2] = { (uint32_t *)RF_DUMP_BANK(0), (uint32_t *)RF_DUMP_BANK(1) };
    st->units = 0; st->late = 0; st->min_margin = 0xFFFFFFFF; st->cb_max = st->copy_max = 0;
    if (cb(bank[0], TXU, ctx)) return 0;                       /* the callback writes straight into the free bank */
    int more = !cb(bank[1] + TXU, TXU, ctx);
    uint32_t *tail = (uint32_t *)RF_DUMP_BANK(2);                /* bank 2 as plain memory (never selected) */
    for (uint32_t k = 0; k < TXTAIL; k++) bank[1][TXU - TXTAIL + k] = bank[0][TXU - TXTAIL + k];
    for (uint32_t k = 0; k < TXTAIL; k++) tail[k] = bank[1][2 * TXU - TXTAIL + k];   /* unit 1's tail */
    REG(CTRL) = 0; REG(DAC) = 0;
    REG(BANK_SEL) = (REG(BANK_SEL) & ~15u) | 1u;
    uint32_t dctrl = 1u << 31 | 16384u;
    REG(DAC) = dctrl;
    uint32_t c0 = ccount_now();
    REG(DAC) = dctrl | 1u << 19;                               /* loop */
    uint64_t cyc = 0; uint32_t cprev = c0;
#define POS() (cyc += (uint32_t)(ccount_now() - cprev), cprev += (uint32_t)(ccount_now() - cprev), (uint32_t)((cyc / CPS) & 0x3FFF))
    for (uint32_t k = 1; ; k++) {                              /* unit k-1 playing from bank (k-1)%2 */
        unsigned cur = (k - 1) & 1, nxt = k & 1;
        uint32_t h = cur * TXU, into;
        for (;;) {
            uint32_t now = ccount_now(); cyc += now - cprev; cprev = now;
            into = ((uint32_t)(cyc / CPS) - h) & 0x3FFF;
            if (into >= TXU - TXTAIL) break;
        }
        if (into >= TXU) { st->late++; st->late_into = into; break; }   /* past the tail: the DAC read stale data (a gap) */
        uint32_t margin = TXU - into; if (margin < st->min_margin) st->min_margin = margin;
        REG(BANK_SEL) = (REG(BANK_SEL) & ~3u) | 1u << nxt;     /* switch: the tail copy makes this seamless */
        st->units++;
        if (!more || st->units + 1 >= units) {                 /* let the last unit play out */
            for (;;) { uint32_t now = ccount_now(); cyc += now - cprev; cprev = now; if ((((uint32_t)(cyc / CPS) - nxt * TXU) & 0x3FFF) >= TXU - 64) break; }
            break;
        }
        uint32_t *fb = bank[cur];
        uint32_t f0 = ccount_now();
        more = !cb(fb + h, TXU, ctx);                          /* unit k+1 -> bank cur, half cur */
        uint32_t f1 = ccount_now();
        uint32_t *d = fb + nxt * TXU + TXU - TXTAIL, *e = fb + h + TXU - TXTAIL;
        for (uint32_t m = 0; m < TXTAIL; m++) { d[m] = tail[m]; tail[m] = e[m]; }   /* unit k's tail -> bank cur; keep unit k+1's */
        uint32_t f2 = ccount_now();
        if (f1 - f0 > st->cb_max) st->cb_max = f1 - f0;
        if (f2 - f1 > st->copy_max) st->copy_max = f2 - f1;
    }
#undef POS
    REG(DAC) = 0;
    REG(BANK_SEL) &= ~15u;
    return st->late ? -1 : 0;
}
