/* P4 ECC accelerator (ecc_hw.h). Flow of ESP-IDF v6.1 esp_hal_security/ecc_hal.c and esp32p4/ecc_ll.h: clock + reset, curve and
 * mode in ECC_MULT_CONF, parameters written to the PX / PY / K / QX / QY / QZ memories (all cleared first), start, wait for
 * CALC_DONE, read back; verification result in CONF b29. Registers: generated regs/ecc_mult.h. NOT VERIFIED ON HARDWARE. */
#include "ecc_hw.h"
#include "clk.h"
#include "cpu.h"
#include "spin.h"
#include "soc.h"
#include "regs/ecc_mult.h"
#include "regs/hp_clkrst.h"
#include "regs/p4reg.h"
#include <string.h>

enum { M_MUL = 0, M_VERIFY = 2, M_VERIFY_MUL = 3, M_ADD = 5, M_MOD_ADD = 8, M_MOD_SUB = 9, M_MOD_MUL = 10, M_MOD_INV = 11 };
static const uint8_t clen[3] = { 24, 32, 48 };
static bool up;

static void init(void)
{
    if (up) return;
    uint32_t t = irq_lock();
    FSET(HP_SYS_CLKRST_REG_CRYPTO_ECC_CLK_EN);
    FSET(HP_SYS_CLKRST_REG_RST_EN_ECC); FCLR(HP_SYS_CLKRST_REG_RST_EN_ECC);
    FCLR(HP_SYS_CLKRST_REG_RST_EN_CRYPTO);
    irq_unlock(t);
    up = true;
}

static void wr(uint32_t mem, const uint8_t *p, int len)
{
    for (int i = 0; i < 48; i += 4) {
        uint32_t w = 0;
        if (p && i < len) memcpy(&w, p + i, len - i >= 4 ? 4 : len - i);
        REG(mem + i) = w;
    }
}
static void rd(uint32_t mem, uint8_t *p, int len) { for (int i = 0; i < len; i += 4) { uint32_t w = REG(mem + i); memcpy(p + i, &w, len - i >= 4 ? 4 : len - i); } }

static int run(ecc_curve_id_t c, int mode, bool mod_n)
{
    REG_FIELD(ECC_MULT_CONF_REG, ECC_MULT_WORK_MODE_S, 4, mode);
    REG_FIELD(ECC_MULT_CONF_REG, ECC_MULT_KEY_LENGTH_S, 2, c);
    if (mod_n) REG_CLR(ECC_MULT_CONF_REG, BIT(ECC_MULT_MOD_BASE_S)); else REG_SET(ECC_MULT_CONF_REG, BIT(ECC_MULT_MOD_BASE_S));
    REG_SET(ECC_MULT_INT_CLR_REG, BIT(ECC_MULT_CALC_DONE_INT_CLR_S));
    REG_SET(ECC_MULT_CONF_REG, BIT(ECC_MULT_START_S));
    SPIN_US(!(REG(ECC_MULT_INT_RAW_REG) & BIT(ECC_MULT_CALC_DONE_INT_RAW_S)), 200000);
    return (REG(ECC_MULT_INT_RAW_REG) & BIT(ECC_MULT_CALC_DONE_INT_RAW_S)) ? 0 : -1;
}
static bool verified(void) { return REG(ECC_MULT_CONF_REG) >> ECC_MULT_VERIFICATION_RESULT_S & 1; }

static void clear_all(void)
{
    wr(ECC_MULT_PX_MEM, 0, 0); wr(ECC_MULT_PY_MEM, 0, 0); wr(ECC_MULT_K_MEM, 0, 0);
    wr(ECC_MULT_QX_MEM, 0, 0); wr(ECC_MULT_QY_MEM, 0, 0); wr(ECC_MULT_QZ_MEM, 0, 0);
}

int ecc_hw_mul(ecc_curve_id_t c, const uint8_t *k, const uint8_t *px, const uint8_t *py, uint8_t *rx, uint8_t *ry)
{
    if ((unsigned)c > 2 || !k || !px || !py || !rx || !ry) return -1;
    init(); int n = clen[c];
    clear_all();
    wr(ECC_MULT_K_MEM, k, n); wr(ECC_MULT_PX_MEM, px, n); wr(ECC_MULT_PY_MEM, py, n);
    if (run(c, M_VERIFY_MUL, false) || !verified()) { memset(rx, 0, n); memset(ry, 0, n); return -1; }      /* verify, then multiply */
    rd(ECC_MULT_PX_MEM, rx, n); rd(ECC_MULT_PY_MEM, ry, n);
    return 0;
}

int ecc_hw_verify(ecc_curve_id_t c, const uint8_t *px, const uint8_t *py)
{
    if ((unsigned)c > 2 || !px || !py) return 0;
    init(); int n = clen[c];
    clear_all();
    wr(ECC_MULT_PX_MEM, px, n); wr(ECC_MULT_PY_MEM, py, n);
    return run(c, M_VERIFY, false) == 0 && verified();
}

int ecc_hw_add(ecc_curve_id_t c, const uint8_t *px, const uint8_t *py, const uint8_t *qx, const uint8_t *qy, uint8_t *rx, uint8_t *ry)
{
    if ((unsigned)c > 2 || !px || !py || !qx || !qy || !rx || !ry) return -1;
    init(); int n = clen[c];
    uint8_t one[48] = { 1 };                                                  /* Q in Jacobian form with Z = 1 */
    clear_all();
    wr(ECC_MULT_PX_MEM, px, n); wr(ECC_MULT_PY_MEM, py, n);
    wr(ECC_MULT_QX_MEM, qx, n); wr(ECC_MULT_QY_MEM, qy, n); wr(ECC_MULT_QZ_MEM, one, n);
    if (run(c, M_ADD, false)) return -1;
    rd(ECC_MULT_PX_MEM, rx, n); rd(ECC_MULT_PY_MEM, ry, n);                   /* affine result in P */
    return 0;
}

int ecc_hw_mod(ecc_curve_id_t c, ecc_mod_op_t op, bool mod_n, const uint8_t *a, const uint8_t *b, uint8_t *r)
{
    if ((unsigned)c > 2 || !a || !r || (op != ECC_MOD_INV && !b)) return -1;
    init(); int n = clen[c];
    static const uint8_t mode[4] = { M_MOD_ADD, M_MOD_SUB, M_MOD_MUL, M_MOD_INV };
    clear_all();
    wr(ECC_MULT_PX_MEM, a, n); wr(ECC_MULT_PY_MEM, op == ECC_MOD_INV ? a : b, n);
    if (run(c, mode[op], mod_n)) return -1;
    rd(op == ECC_MOD_ADD || op == ECC_MOD_SUB ? ECC_MULT_PX_MEM : ECC_MULT_PY_MEM, r, n);       /* add / sub leave the result in PX, mul / inverse in PY */
    return 0;
}
