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

int rf_dump_burst(uint32_t rate, uint32_t n)
{
    volatile uint32_t *w = (volatile uint32_t *)RF_DUMP_BURST_ADDR;
    if (n > RF_DUMP_MAX) n = RF_DUMP_MAX;
    for (uint32_t i = 0; i < n; i++) w[i] = UNWRITTEN;
    uint32_t owner = REG(BANK_SEL), ctrl = 1u << 31 | rate | n;
    REG(CTRL) = 0;
    REG(CONFIG) = 0x000C2040u;
    REG(BANK_SEL) = owner | 4u;                 /* keep a playing DAC's bank */
    REG(CTRL) = ctrl;
    REG(CTRL) = ctrl | 1u << 19;
    REG(CTRL) = ctrl;
    uint32_t spin = 0;
    while (!(REG(CTRL) & 1u << 18) && ++spin < 10000000u) {}
    REG(CTRL) = 0;
    REG(BANK_SEL) = owner;
    return w[n - 1] == UNWRITTEN ? -1 : 0;
}
