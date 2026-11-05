/* P4 Event Task Matrix (etm.h). Flow of ESP-IDF v6.1 hal etm_ll.h (esp32p4): APB + system clock gates, reset pulse, per-channel
 * event / task id registers (8 bytes apart), channel enable through the SET / CLR registers (channels 0..31, 32..49). Registers:
 * regs/etm.h. NOT VERIFIED ON HARDWARE. */
#include "etm.h"
#include "clk.h"
#include "cpu.h"
#include "soc.h"
#include "regs/etm.h"
#include "regs/hp_clkrst.h"
#include "regs/p4reg.h"

static bool up;

int etm_init(void)
{
    if (up) return 0;
    uint32_t t = irq_lock();
    FSET(HP_SYS_CLKRST_REG_ETM_APB_CLK_EN); FSET(HP_SYS_CLKRST_REG_ETM_SYS_CLK_EN);
    FSET(HP_SYS_CLKRST_REG_RST_EN_ETM); FCLR(HP_SYS_CLKRST_REG_RST_EN_ETM);
    irq_unlock(t);
    up = true;
    return 0;
}

int etm_connect(int ch, int ev, int task)
{
    if ((unsigned)ch >= ETM_CHANNELS || ev < 0 || ev > 255 || task < 0 || task > 255 || etm_init()) return -1;
    REG(SOC_ETM_CH0_EVT_ID_REG + 8 * ch) = ev;
    REG(SOC_ETM_CH0_TASK_ID_REG + 8 * ch) = task;
    REG(ch < 32 ? SOC_ETM_CH_ENA_AD0_SET_REG : SOC_ETM_CH_ENA_AD1_SET_REG) = BIT(ch & 31);
    return 0;
}

void etm_disconnect(int ch)
{
    if ((unsigned)ch >= ETM_CHANNELS || !up) return;
    REG(ch < 32 ? SOC_ETM_CH_ENA_AD0_CLR_REG : SOC_ETM_CH_ENA_AD1_CLR_REG) = BIT(ch & 31);
}

bool etm_enabled(int ch) { return up && (unsigned)ch < ETM_CHANNELS && (REG(ch < 32 ? SOC_ETM_CH_ENA_AD0_REG : SOC_ETM_CH_ENA_AD1_REG) >> (ch & 31) & 1); }
