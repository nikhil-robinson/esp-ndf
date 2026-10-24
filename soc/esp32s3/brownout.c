/* Brownout + glitch detection, ESP-IDF sequence (hal/brownout_hal.c, esp32s3 bootloader_soc.c): software control
 * (analog reset off, FIB_SEL cleared), interrupt wait 2, flash + RF power-down on detection, counter cleared, reset
 * wait 0x3FF, threshold in the BOD analog block (regi2c 0x61 reg 5 bits 2:0). */
#include "brownout.h"
#include "regi2c.h"
#include "irq.h"
#include "rtc_irq.h"
#include "system.h"
#include "xtensa.h"

#define BROWN_OUT   0x600080E8u    /* b31 det b30 ena b29 cnt_clr b28 ana_rst b27 rst_sel b26 rst_ena b25:16 rst_wait
                                      b15 pd_rf b14 close_flash b13:4 int_wait */
#define FIB_SEL     0x60008148u    /* b0 glitch, b1 BOD, b2 super WDT reset: 0 = register control */
#define ANA_CONF    0x60008034u    /* b20 glitch reset enable */
#define INT_ENA     0x60008040u    /* b9 brownout */
#define INT_CLR     0x6000804Cu
#define REGI2C_BOD  0x61

static void (*user_fn)(void);

static void bod_isr(void *arg)
{
    (void)arg;
    REG(INT_CLR) = BIT(9);
    if (core_id() == 0) REG_SET(SYSTEM_CORE1_CTRL0, BIT(0));       /* stop core 1 (as IDF's handler) */
    if (user_fn) user_fn();
    REG(0x50001FECu) = 0x52430000u | RESET_BROWNOUT;              /* next boot reads the real cause (system.c) */
    sys_restart();
}

static void bod_config(int level, bool reset)
{
    REG_CLR(FIB_SEL, BIT(1));
    uint32_t v = (REG(BROWN_OUT) & ~(BIT(28) | BIT(27) | BIT(26) | (0x3FFu << 16) | (0x3FFu << 4)))
               | BIT(15) | BIT(14) | 2u << 4 | 0x3FFu << 16 | BIT(27) | (reset ? BIT(26) : 0);
    REG(BROWN_OUT) = v | BIT(29); REG(BROWN_OUT) = v;               /* counter cleared */
    /* the threshold (analog) survives system resets and is cleared at power-on together with RTC memory: written
     * only when the note in RTC memory says it differs (the slow analog bus write is most of the setup time) */
    if (REG(0x50001FE8u) != 0x424F4400u + (uint32_t)level) {
        regi2c_write_mask(REGI2C_BOD, 5, 2, 0, (uint8_t)level);
        REG(0x50001FE8u) = 0x424F4400u + (uint32_t)level;
    }
    REG_SET(BROWN_OUT, BIT(30));
    REG(INT_CLR) = BIT(9);
}

/* interrupt mode, as IDF's default (ESP_SYSTEM_BROWNOUT_INTR) */
int bod_start(int level, void (*fn)(void))
{
    if (level < 1 || level > 7) return -1;
    bod_config(level, false);
    user_fn = fn;
    if (rtc_irq_on(BIT(9), bod_isr, 0) < 0) return -1;   /* shared RTC interrupt (soc/esp32s3/rtc_irq.c) */
    return 0;
}

void bod_reset_mode(int level) { if (level >= 1 && level <= 7) { bod_config(level, true); REG_CLR(INT_ENA, BIT(9)); } }   /* hardware reset (IDF non-interrupt mode) */

void bod_stop(void) { REG_CLR(INT_ENA, BIT(9)); REG_CLR(BROWN_OUT, BIT(30)); }
bool bod_low(void) { return REG(BROWN_OUT) & BIT(31); }

void glitch_reset(bool on)
{
    REG_CLR(FIB_SEL, BIT(0));
    if (on) REG_SET(ANA_CONF, BIT(20)); else REG_CLR(ANA_CONF, BIT(20));
}
