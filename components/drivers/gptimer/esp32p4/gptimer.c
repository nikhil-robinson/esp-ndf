/* P4 GPTimer: the TIMG timer registers are the S3's; the clock source moved to HP_SYS_CLKRST (PERI_CLK_CTRL20/21,
 * IDF timer_ll_set_clock_source: 0 XTAL, 1 RC_FAST, 2 PLL_F80M). XTAL 40 MHz only: it runs at every CPU clock.
 * Interrupt sources TG0_T0 46, TG0_T1 47, TG1_T0 49, TG1_T1 50. */
#include "soc.h"
#define GPT_SRCS { SRC_TG0_T0, SRC_TG0_T1, SRC_TG1_T0, SRC_TG1_T1 }
#define GPT_APB_HZ 0u
static uint32_t gpt_clk_sel(int id)
{
    static const uint8_t reg[4] = { 0x94, 0x94, 0x98, 0x98 }, sel[4] = { 22, 25, 20, 23 };   /* src b+1:b, enable b+2 */
    uint32_t a = HP_CLKRST_BASE + reg[id];
    REG(a) = (REG(a) & ~(3u << sel[id])) | BIT(sel[id] + 2);
    return 0;
}
#define GPT_CLK_SEL(id, xtal) ((void)(xtal), gpt_clk_sel(id))
#include "../gptimer.c"
