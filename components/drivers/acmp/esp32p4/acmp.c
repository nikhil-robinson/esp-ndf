/* P4 analog comparators (acmp.h), TRM chapter 66 programming procedure: XPD_COMPn, the pads' digital functions off, MODE_COMPn
 * (0 internal / 1 external reference), DREF_COMPn; edge events through the GPIO interrupt block (COMPn_NEG / POS / ALL raw
 * status). Registers: regs/lp_sys.h (PAD_COMPn), regs/gpio.h. NOT VERIFIED ON HARDWARE. */
#include "acmp.h"
#include "soc.h"
#include "regs/lp_sys.h"
#include "regs/gpio.h"
#include "regs/p4reg.h"

static const uint8_t main_pad[2] = { 52, 54 }, ref_pad[2] = { 51, 53 };

static void pad_analog(int pin) { REG_CLR(IO_MUX_BASE + 4 + pin * 4, BIT(9) | BIT(8) | BIT(7)); }       /* IE, pull-up, pull-down off */

int acmp_enable(int u, acmp_ref_t ref, int tenths)
{
    if ((unsigned)u > 1 || (unsigned)tenths > 7) return -1;
    pad_analog(main_pad[u]);
    if (ref == ACMP_REF_EXTERNAL) pad_analog(ref_pad[u]);
    uint32_t r = u ? LP_SYSTEM_REG_PAD_COMP1_REG : LP_SYSTEM_REG_PAD_COMP0_REG;
    REG(r) = (uint32_t)tenths | (ref == ACMP_REF_EXTERNAL ? BIT(3) : 0);       /* DREF b2:0, MODE b3 */
    REG_SET(r, BIT(4));                                                          /* XPD */
    REG(GPIO_INT_CLR_REG) = 7u << (3 * u);
    return 0;
}

void acmp_disable(int u) { if ((unsigned)u < 2) REG_CLR(u ? LP_SYSTEM_REG_PAD_COMP1_REG : LP_SYSTEM_REG_PAD_COMP0_REG, BIT(4)); }

uint32_t acmp_events(int u)
{
    if ((unsigned)u > 1) return 0;
    uint32_t r = REG(GPIO_INT_RAW_REG) >> (3 * u) & 7;                           /* NEG, POS, ALL */
    return ((r & 1) ? ACMP_EV_NEG : 0) | ((r & 2) ? ACMP_EV_POS : 0) | ((r & 4) ? ACMP_EV_ANY : 0);
}

void acmp_clear(int u, uint32_t ev)
{
    if ((unsigned)u > 1) return;
    REG(GPIO_INT_CLR_REG) = (((ev & ACMP_EV_NEG) ? 1u : 0) | ((ev & ACMP_EV_POS) ? 2u : 0) | ((ev & ACMP_EV_ANY) ? 4u : 0)) << (3 * u);
}
