/* ULP-RISC-V start / stop, IDF ulp_riscv_config_and_run (S3 branch, IDF-4510 order) + ulp_set_wakeup_period. */
#include "ulp.h"
#include "rtcclk.h"
#include "sys.h"

#define R(o)            (0x60008000u + (o))
#define RESET_STATE     R(0x38)          /* b13: CPU0 reset vector 1 = ROM */
#define ULP_TIMER       R(0xFC)          /* b31 SLP_TIMER_EN */
#define ULP_CTRL        R(0x100)         /* b30 FORCE_START_TOP */
#define COCPU_CTRL      R(0x104)
#define ULP_TIMER1      R(0x134)         /* b31:8 sleep cycles */
#define INT_CLR         R(0x4C)
#define CLK_CONF        R(0x74)          /* b29 FAST_CLK_RTC_SEL: 1 = RC_FAST (the ULP's clock, as IDF) */
#define CO_CLK_FO       BIT(0)
#define CO_SHUT         BIT(13)
#define CO_RESET_EN     BIT(22)
#define CO_SEL          BIT(23)
#define CO_DONE_FORCE   BIT(24)
#define CO_DONE         BIT(25)
#define CO_CLKGATE      BIT(27)

int ulp_load(const void *bin, size_t len)
{
    if (!NDF_ULP_RESERVE || len > NDF_ULP_RESERVE + 0x10) return -1;
    ulp_stop();
    REG_SET(RESET_STATE, BIT(13));                          /* resets via the ROM from now on: 0x50000000 is ULP code */
    volatile uint32_t *m = (volatile uint32_t *)0x50000000u;  /* RTC slow memory: microseconds per access, so words */
    const uint8_t *b = bin;
    uint32_t n = (len + 3) / 4, all = (NDF_ULP_RESERVE + 0x10) / 4;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t w = 0;
        for (int k = 0; k < 4 && 4 * i + k < len; k++) w |= (uint32_t)b[4 * i + k] << (8 * k);
        m[i] = w;
    }
    for (uint32_t i = n; i < all; i++) m[i] = 0;
    return 0;
}

int ulp_run(uint32_t period_us)
{
    uint64_t cyc = (uint64_t)period_us * rtc_slow_hz() / 1000000u;
    if (!cyc || cyc > 0xFFFFFF) return -1;
    REG_SET(CLK_CONF, BIT(29));                             /* RTC fast clock from RC_FAST: the ULP does not start on XTAL/2 */
    REG_SET(COCPU_CTRL, CO_RESET_EN);                       /* reset the coprocessor */
    REG_SET(COCPU_CTRL, CO_CLK_FO);                         /* trap signal has no stable reset value */
    REG_CLR(ULP_TIMER, BIT(31));
    delay_us(20);                                           /* >= 1 slow-clock cycle */
    REG_CLR(COCPU_CTRL, CO_RESET_EN);                       /* reset pulse over: held in reset, the timer cannot start it */
    REG_SET(COCPU_CTRL, CO_DONE_FORCE);
    REG_SET(COCPU_CTRL, CO_CLKGATE);
    REG_CLR(COCPU_CTRL, CO_DONE);                           /* left set by ulp_stop: the timer won't start a DONE core */
    REG(ULP_TIMER1) = (REG(ULP_TIMER1) & 0xFF) | (uint32_t)cyc << 8;
    REG_CLR(ULP_CTRL, BIT(30));
    REG_SET(ULP_TIMER, BIT(31));                            /* start the ULP timer */
    REG_CLR(COCPU_CTRL, CO_SEL);                            /* RISC-V as the timer target, last (IDF-4510) */
    delay_us(20);
    REG(INT_CLR) = BIT(13) | BIT(17) | BIT(5);              /* COCPU, COCPU trap, ULP_CP: spurious at start */
    return 0;
}

void ulp_stop(void)
{
    REG_CLR(ULP_TIMER, BIT(31));
    REG_SET(COCPU_CTRL, CO_DONE);
    REG_SET(COCPU_CTRL, CO_RESET_EN);
}

/* ULP-FSM (IDF ulp_fsm/ulp.c, S3 branch): program loaded with ulp_load, entry = word index */
int ulp_fsm_run(uint32_t entry_word, uint32_t period_us)
{
    uint64_t cyc = (uint64_t)period_us * rtc_slow_hz() / 1000000u;
    if (!cyc || cyc > 0xFFFFFF || entry_word > 0x7FF) return -1;
    REG_SET(CLK_CONF, BIT(29));                             /* RTC fast clock from RC_FAST */
    REG_SET(ULP_CTRL, BIT(29)); delay_us(20); REG_CLR(ULP_CTRL, BIT(29));   /* ULP_CP_RESET pulse */
    REG_CLR(ULP_TIMER, BIT(31));
    delay_us(10);
    REG(ULP_TIMER) = (REG(ULP_TIMER) & ~0x7FFu) | entry_word;
    REG_SET(COCPU_CTRL, CO_SEL);                            /* timer target: the FSM */
    REG_SET(ULP_CTRL, BIT(28));                             /* ULP clock on */
    REG_CLR(COCPU_CTRL, CO_DONE_FORCE);                     /* the FSM sends DONE */
    REG_SET(COCPU_CTRL, CO_CLKGATE);
    REG(INT_CLR) = BIT(13) | BIT(17) | BIT(5);
    REG(ULP_TIMER1) = (REG(ULP_TIMER1) & 0xFF) | (uint32_t)cyc << 8;
    REG_CLR(ULP_CTRL, BIT(30));
    REG_SET(ULP_TIMER, BIT(31));
    return 0;
}
