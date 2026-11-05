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

#if NDF_P4_REV < 3     /* v1.x: 32-byte operand memories at a 0x20 stride (hw_ver1 ecc_mult_reg.h), no P-384 */
#define E_K  (ECC_MULT_BASE + 0x100)
#define E_PX (ECC_MULT_BASE + 0x120)
#define E_PY (ECC_MULT_BASE + 0x140)
#define E_QX (ECC_MULT_BASE + 0x160)
#define E_QY (ECC_MULT_BASE + 0x180)
#define E_QZ (ECC_MULT_BASE + 0x1A0)
#define E_SZ 32
#define E_P384 0
#define E_S_KEY 2                  /* CONF fields: key length (1 bit), mod base, work mode, as hw_ver1 ecc_mult_reg.h */
#define E_W_KEY 1
#define E_S_MOD 3
#define E_S_MODE 4
#else
#define E_K  ECC_MULT_K_MEM
#define E_PX ECC_MULT_PX_MEM
#define E_PY ECC_MULT_PY_MEM
#define E_QX ECC_MULT_QX_MEM
#define E_QY ECC_MULT_QY_MEM
#define E_QZ ECC_MULT_QZ_MEM
#define E_SZ 48
#define E_P384 1
#define E_S_KEY ECC_MULT_KEY_LENGTH_S
#define E_W_KEY 2
#define E_S_MOD ECC_MULT_MOD_BASE_S
#define E_S_MODE ECC_MULT_WORK_MODE_S
#endif
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
    for (int i = 0; i < E_SZ; i += 4) {
        uint32_t w = 0;
        if (p && i < len) memcpy(&w, p + i, len - i >= 4 ? 4 : len - i);
        REG(mem + i) = w;
    }
}
static void rd(uint32_t mem, uint8_t *p, int len) { for (int i = 0; i < len; i += 4) { uint32_t w = REG(mem + i); memcpy(p + i, &w, len - i >= 4 ? 4 : len - i); } }

static int run(ecc_curve_id_t c, int mode, bool mod_n)
{
    REG_FIELD(ECC_MULT_CONF_REG, E_S_MODE, 4, mode);
    REG_FIELD(ECC_MULT_CONF_REG, E_S_KEY, E_W_KEY, c);
    if (mod_n) REG_CLR(ECC_MULT_CONF_REG, BIT(E_S_MOD)); else REG_SET(ECC_MULT_CONF_REG, BIT(E_S_MOD));
    REG_SET(ECC_MULT_INT_CLR_REG, BIT(ECC_MULT_CALC_DONE_INT_CLR_S));
    REG_SET(ECC_MULT_CONF_REG, BIT(ECC_MULT_START_S));
    SPIN_WHILE(!(REG(ECC_MULT_INT_RAW_REG) & BIT(ECC_MULT_CALC_DONE_INT_RAW_S)), 200000);
    return (REG(ECC_MULT_INT_RAW_REG) & BIT(ECC_MULT_CALC_DONE_INT_RAW_S)) ? 0 : -1;
}
static bool verified(void) { return REG(ECC_MULT_CONF_REG) >> ECC_MULT_VERIFICATION_RESULT_S & 1; }

static void clear_all(void)
{
    /* the curve is set before any parameter is written (ecc_hal_set_curve first): the memories follow the key length */
    wr(E_PX, 0, 0); wr(E_PY, 0, 0); wr(E_K, 0, 0);
    wr(E_QX, 0, 0); wr(E_QY, 0, 0); wr(E_QZ, 0, 0);
}

int ecc_hw_mul(ecc_curve_id_t c, const uint8_t *k, const uint8_t *px, const uint8_t *py, uint8_t *rx, uint8_t *ry)
{
    if ((unsigned)c > 2 || (c == ECC_P384 && !E_P384) || !k || !px || !py || !rx || !ry) return -1;
    init(); int n = clen[c]; REG_FIELD(ECC_MULT_CONF_REG, E_S_KEY, E_W_KEY, c);
    clear_all();
    wr(E_K, k, n); wr(E_PX, px, n); wr(E_PY, py, n);
    if (run(c, M_VERIFY_MUL, false) || !verified()) { memset(rx, 0, n); memset(ry, 0, n); return -1; }      /* verify, then multiply */
    rd(E_PX, rx, n); rd(E_PY, ry, n);
    return 0;
}

int ecc_hw_verify(ecc_curve_id_t c, const uint8_t *px, const uint8_t *py)
{
    if ((unsigned)c > 2 || (c == ECC_P384 && !E_P384) || !px || !py) return 0;
    init(); int n = clen[c]; REG_FIELD(ECC_MULT_CONF_REG, E_S_KEY, E_W_KEY, c);
    clear_all();
    wr(E_PX, px, n); wr(E_PY, py, n);
    return run(c, M_VERIFY, false) == 0 && verified();
}

int ecc_hw_add(ecc_curve_id_t c, const uint8_t *px, const uint8_t *py, const uint8_t *qx, const uint8_t *qy, uint8_t *rx, uint8_t *ry)
{
    if ((unsigned)c > 2 || (c == ECC_P384 && !E_P384) || !px || !py || !qx || !qy || !rx || !ry) return -1;
    init(); int n = clen[c]; REG_FIELD(ECC_MULT_CONF_REG, E_S_KEY, E_W_KEY, c);
    uint8_t one[48] = { 1 };                                                  /* Q in Jacobian form with Z = 1 */
    clear_all();
    wr(E_PX, px, n); wr(E_PY, py, n);
    wr(E_QX, qx, n); wr(E_QY, qy, n); wr(E_QZ, one, n);
    if (run(c, M_ADD, false)) return -1;
    rd(E_PX, rx, n); rd(E_PY, ry, n);                   /* affine result in P */
    return 0;
}

int ecc_hw_mod(ecc_curve_id_t c, ecc_mod_op_t op, bool mod_n, const uint8_t *a, const uint8_t *b, uint8_t *r)
{
    if ((unsigned)c > 2 || (c == ECC_P384 && !E_P384) || !a || !r || (op != ECC_MOD_INV && !b)) return -1;
    init(); int n = clen[c]; REG_FIELD(ECC_MULT_CONF_REG, E_S_KEY, E_W_KEY, c);
    static const uint8_t mode[4] = { M_MOD_ADD, M_MOD_SUB, M_MOD_MUL, M_MOD_INV };
    clear_all();
    uint8_t one[48] = { 1 };                                                    /* mode 11 leaves PX * PY^-1 in PY: PX = 1 gives the inverse */
    if (op == ECC_MOD_INV) { wr(E_PX, one, n); wr(E_PY, a, n); } else { wr(E_PX, a, n); wr(E_PY, b, n); }
    if (run(c, mode[op], mod_n)) return -1;
    rd(op == ECC_MOD_ADD || op == ECC_MOD_SUB ? E_PX : E_PY, r, n);       /* add / sub leave the result in PX, mul / inverse in PY */
    return 0;
}
