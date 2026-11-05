/* P4 ECDSA accelerator (ecdsa_hw.h). Flow of ESP-IDF v6.1 esp_hal_security/ecdsa_hal.c and esp32p4/ecdsa_ll.h: curve and work
 * mode in CONF, hash supplied by software (SOFTWARE_SET_Z), START, wait for the LOAD state, write the operands, LOAD_DONE,
 * wait for GET / IDLE, read, GET_DONE. The eFuse key block is selected in the eFuse controller's ECDSA config register and the
 * block data re-read (READ_CMD only: this file never issues a programming command). Registers: regs/ecdsa.h, regs/efuse.h. */
#include "ecdsa_hw.h"
#include "clk.h"
#include "cpu.h"
#include "spin.h"
#include "soc.h"
#include "regs/ecdsa.h"
#include "regs/efuse.h"
#include "regs/hp_clkrst.h"
#include "regs/p4reg.h"
#include <string.h>

enum { ST_IDLE = 0, ST_LOAD = 1, ST_GET = 2 };                             /* ECDSA_STATE.BUSY encodes: 0 idle, 1 load, 2 get */
enum { MODE_VERIFY = 0, MODE_SIGN = 1, MODE_EXPORT = 2 };
static const uint8_t clen[3] = { 24, 32, 48 };
static bool up;

static void init(void)
{
    if (up) return;
    uint32_t t = irq_lock();
    FSET(HP_SYS_CLKRST_REG_CRYPTO_ECDSA_CLK_EN);
    FSET(HP_SYS_CLKRST_REG_RST_EN_ECDSA); FCLR(HP_SYS_CLKRST_REG_RST_EN_ECDSA); FCLR(HP_SYS_CLKRST_REG_RST_EN_CRYPTO);
    irq_unlock(t);
    SPIN_US(REG(ECDSA_STATE_REG) & 3, 10000);
    up = true;
}
static uint32_t st(void) { return REG(ECDSA_STATE_REG) & 3; }
static void wr(uint32_t mem, const uint8_t *p, int len) { for (int i = 0; i < len; i += 4) { uint32_t w; memcpy(&w, p + i, 4); REG(mem + i) = w; } }
static void rd(uint32_t mem, uint8_t *p, int len) { for (int i = 0; i < len; i += 4) { uint32_t w = REG(mem + i); memcpy(p + i, &w, 4); } }

static void efuse_read(void)                                              /* reload the key blocks after the block number changed (hal efuse_hal_read) */
{
    REG_FIELD(EFUSE_CONF_REG, EFUSE_OP_CODE_S, EFUSE_OP_CODE_W, 0x5A5A);
    REG_SET(EFUSE_CMD_REG, BIT(EFUSE_READ_CMD_S));
    SPIN_US(REG(EFUSE_CMD_REG) & BIT(EFUSE_READ_CMD_S), 100000);
    SPIN_US(REG(EFUSE_CMD_REG) & BIT(EFUSE_READ_CMD_S), 100000);
}
static void select_key(ecc_curve_id_t c, int blk)
{
    if (c == ECC_P192) REG_FIELD(EFUSE_ECDSA_REG, EFUSE_CFG_ECDSA_P192_BLK_S, 4, blk);
    else if (c == ECC_P256) REG_FIELD(EFUSE_ECDSA_REG, EFUSE_CFG_ECDSA_P256_BLK_S, 4, blk);
    else { REG_FIELD(EFUSE_ECDSA_REG, EFUSE_CFG_ECDSA_P384_L_BLK_S, 4, blk & 15); REG_FIELD(EFUSE_ECDSA_REG, EFUSE_CFG_ECDSA_P384_H_BLK_S, 4, blk >> 4 & 15); }
    REG_SET(EFUSE_WR_TIM_CONF0_RS_BYPASS_REG, BIT(EFUSE_UPDATE_S));
    efuse_read();
}
static void conf(ecc_curve_id_t c, int mode, bool sw_z)
{
    REG_FIELD(ECDSA_CONF_REG, ECDSA_WORK_MODE_S, 2, mode);
    REG_FIELD(ECDSA_CONF_REG, ECDSA_ECC_CURVE_S, 2, c);
    if (sw_z) REG_SET(ECDSA_CONF_REG, BIT(ECDSA_SOFTWARE_SET_Z_S)); else REG_CLR(ECDSA_CONF_REG, BIT(ECDSA_SOFTWARE_SET_Z_S));
    REG_CLR(ECDSA_CONF_REG, BIT(ECDSA_DETERMINISTIC_K_S));                  /* k from the TRNG */
}
static int phase(uint32_t want)
{
    SPIN_US(st() != want, 2000000);
    return st() == want ? 0 : -1;
}

int ecdsa_hw_verify(ecc_curve_id_t c, const uint8_t *hash, const uint8_t *r, const uint8_t *s, const uint8_t *qx, const uint8_t *qy)
{
    if ((unsigned)c > 2 || !hash || !r || !s || !qx || !qy) return -1;
    init(); int n = clen[c];
    if (st() != ST_IDLE) return -1;
    conf(c, MODE_VERIFY, true);
    REG_SET(ECDSA_START_REG, BIT(ECDSA_START_S));
    if (phase(ST_LOAD)) return -1;
    wr(ECDSA_Z_MEM, hash, n); wr(ECDSA_R_MEM, r, n); wr(ECDSA_S_MEM, s, n); wr(ECDSA_QAX_MEM, qx, n); wr(ECDSA_QAY_MEM, qy, n);
    REG_SET(ECDSA_START_REG, BIT(ECDSA_LOAD_DONE_S));
    if (phase(ST_IDLE)) return -1;
    return (REG(ECDSA_RESULT_REG) & BIT(ECDSA_OPERATION_RESULT_S)) ? 0 : -1;
}

int ecdsa_hw_sign(ecc_curve_id_t c, int blk, const uint8_t *hash, uint8_t *r, uint8_t *s)
{
    if ((unsigned)c > 2 || !hash || !r || !s) return -1;
    init(); int n = clen[c];
    if (st() != ST_IDLE) return -1;
    select_key(c, blk);
    conf(c, MODE_SIGN, true);
    REG_SET(ECDSA_START_REG, BIT(ECDSA_START_S));
    if (phase(ST_LOAD)) return -1;
    wr(ECDSA_Z_MEM, hash, n);
    REG_SET(ECDSA_START_REG, BIT(ECDSA_LOAD_DONE_S));
    if (phase(ST_GET)) return -1;
    rd(ECDSA_R_MEM, r, n); rd(ECDSA_S_MEM, s, n);
    REG_SET(ECDSA_START_REG, BIT(ECDSA_GET_DONE_S));
    return phase(ST_IDLE) || !(REG(ECDSA_RESULT_REG) & BIT(ECDSA_OPERATION_RESULT_S)) ? -1 : 0;
}

int ecdsa_hw_pubkey(ecc_curve_id_t c, int blk, uint8_t *qx, uint8_t *qy)
{
    if ((unsigned)c > 2 || !qx || !qy) return -1;
    init(); int n = clen[c];
    if (st() != ST_IDLE) return -1;
    select_key(c, blk);
    conf(c, MODE_EXPORT, false);
    REG_SET(ECDSA_START_REG, BIT(ECDSA_START_S));
    if (phase(ST_LOAD)) return -1;
    REG_SET(ECDSA_START_REG, BIT(ECDSA_LOAD_DONE_S));
    if (phase(ST_GET)) return -1;
    rd(ECDSA_QAX_MEM, qx, n); rd(ECDSA_QAY_MEM, qy, n);
    REG_SET(ECDSA_START_REG, BIT(ECDSA_GET_DONE_S));
    return phase(ST_IDLE);
}
