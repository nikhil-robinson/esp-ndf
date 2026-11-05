/* P4 brownout detector (brownout.h): LP_ANA_PERI mode 0 (ESP-IDF v6.1 esp_hal_pmu brownout_hal.c / brownout_ll.h): software
 * control (analog reset off), interrupt wait 2, flash + RF power-down on detection, counter cleared, reset wait 0x3FF,
 * threshold in the BOD analog block (regi2c 0x6a reg 9, bits 4:2 low and 7:5 high). Levels 5 / 6 / 7 are IDF's 2.42 / 2.52 /
 * 2.60 V; 1..4 are the lower raw thresholds. The interrupt is LP_ANAPERI (source 8), shared with the other LP analog
 * events: the handler only reacts to the BOD bit (31). Clock-glitch reset is the CK_GLITCH_CNTL enable (IDF has no code for
 * it on the P4 yet: the register is in the TRM block, bit 31). */
#include "brownout.h"
#include "regi2c.h"
#include "irq.h"
#include "system.h"
#include "cpu.h"
#include "soc.h"

#define LP_ANA          0x50113000u
#define BOD0            (LP_ANA + 0x00)    /* b6 close flash b7 pd rf b17:8 int wait b27:18 reset wait b28 cnt clr b29 int ena b30 reset sel b31 reset ena */
#define BOD1            (LP_ANA + 0x04)    /* b31 analog reset enable */
#define CK_GLITCH       (LP_ANA + 0x14)    /* b31 reset on a clock glitch (ck_glitch_cntl is the 6th register) */
#define FIB_ENABLE      (LP_ANA + 0x1C)    /* b1 BOD analog-fib: 0 = register control */
#define INT_ST          (LP_ANA + 0x24)
#define INT_ENA         (LP_ANA + 0x28)
#define INT_CLR         (LP_ANA + 0x2C)
#define BOD_INT         BIT(31)
#define SRC_LP_ANAPERI  8
#define I2C_BOD         0x6A

static void (*user_fn)(void);

static void bod_isr(void *arg)
{
    (void)arg;
    if (!(REG(INT_ST) & BOD_INT)) return;
    REG(INT_CLR) = BOD_INT;
    if (user_fn) user_fn();
    sys_restart();
}

static void bod_config(int level, bool reset)
{
    REG_CLR(FIB_ENABLE, BIT(1));
    REG_CLR(BOD1, BIT(31));                                   /* analog reset off */
    uint32_t v = (REG(BOD0) & ~(BIT(31) | BIT(30) | BIT(29) | (0x3FFu << 18) | (0x3FFu << 8)))
               | BIT(7) | BIT(6) | 2u << 8 | 0x3FFu << 18 | (reset ? BIT(31) : 0);   /* reset_sel 0 = system reset */
    REG(BOD0) = v | BIT(28); REG(BOD0) = v;                  /* counter cleared */
    regi2c_write_mask(I2C_BOD, 9, 4, 2, (uint8_t)level);
    regi2c_write_mask(I2C_BOD, 9, 7, 5, (uint8_t)level);
    REG_SET(BOD0, BIT(29));
    REG(INT_CLR) = BOD_INT;
}

int bod_start(int level, void (*fn)(void))
{
    if (level < 1 || level > 7) return -1;
    bod_config(level, false);
    user_fn = fn;
    if (irq_attach(SRC_LP_ANAPERI, 5, bod_isr, 0) < 0) return -1;
    REG_SET(INT_ENA, BOD_INT);
    return 0;
}

void bod_reset_mode(int level) { if (level >= 1 && level <= 7) { bod_config(level, true); REG_CLR(INT_ENA, BOD_INT); } }
void bod_stop(void) { REG_CLR(INT_ENA, BOD_INT); REG_CLR(BOD0, BIT(29)); }
bool bod_low(void) { return REG(LP_ANA + 0x20) & BOD_INT; }   /* INT_RAW */

void glitch_reset(bool on)
{
    if (on) REG_SET(CK_GLITCH, BIT(31)); else REG_CLR(CK_GLITCH, BIT(31));
}
