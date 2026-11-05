/* P4 Digital Signature (ds.h). Flow of ESP-IDF v6.1 esp_security/src/esp_ds.c (HAL variant) and esp_hal_security/ds_hal.c +
 * esp32p4/ds_ll.h + hmac_hal.c: DS, HMAC and SHA clocks on, HMAC started with the "downstream DS" purpose on the chosen eFuse
 * key block, DS started and given 1.1 ms to accept the key, the encrypted parameters written into Y / M / Rb / BOX memories,
 * IV, message into X, CONTINUE, wait, check (MD / padding), read Z, FINISH, invalidate the HMAC DS key. */
#include "ds.h"
#include "clk.h"
#include "cpu.h"
#include "spin.h"
#include "soc.h"
#include "regs/ds.h"
#include "regs/hmac.h"
#include "regs/hp_clkrst.h"
#include "regs/p4reg.h"
#include <string.h>

#define PURPOSE_DOWN_DS 7
static void fence(void) { __asm__ volatile("fence" ::: "memory"); }
static void hidle(void) { SPIN_US(REG(HMAC_QUERY_BUSY_REG) != 0, 100000); }

int ds_sign(const ds_data_t *d, int key, const uint8_t *msg, uint8_t *sig)
{
    if (!d || !msg || !sig || (unsigned)key > 5 || (d->rsa_length != 31 && d->rsa_length != 63 && d->rsa_length != 95 && d->rsa_length != 127)) return -1;
    size_t n = (d->rsa_length + 1) * 4;
    uint32_t t = irq_lock();
    FSET(HP_SYS_CLKRST_REG_CRYPTO_DS_CLK_EN); FSET(HP_SYS_CLKRST_REG_CRYPTO_HMAC_CLK_EN); FSET(HP_SYS_CLKRST_REG_CRYPTO_SHA_CLK_EN);
    FSET(HP_SYS_CLKRST_REG_RST_EN_DS); FCLR(HP_SYS_CLKRST_REG_RST_EN_DS); FCLR(HP_SYS_CLKRST_REG_RST_EN_CRYPTO);
    irq_unlock(t);
    hidle(); REG(HMAC_SET_START_REG) = 1;                                     /* HMAC derives the DS key from the eFuse block */
    hidle(); REG(HMAC_SET_PARA_PURPOSE_REG) = PURPOSE_DOWN_DS; REG(HMAC_SET_PARA_KEY_REG) = key; REG(HMAC_SET_PARA_FINISH_REG) = 1;
    hidle();
    if (REG(HMAC_QUERY_ERROR_REG)) { REG(HMAC_SET_RESULT_FINISH_REG) = 2; return -1; }
    hidle();
    REG(DS_SET_START_REG) = 1;
    SPIN_US(REG(DS_QUERY_BUSY_REG) > 0, 1100);                                /* key check window (SOC_DS_KEY_CHECK_MAX_WAIT_US) */
    if (REG(DS_QUERY_BUSY_REG) > 0) { REG(DS_SET_FINISH_REG) = 1; SPIN_US(REG(DS_QUERY_BUSY_REG) > 0, 100000); return -1; }
    const uint8_t *from = d->c;                                               /* Y, M, r_bar at full length, then the box (MD, M', L, padding) */
    memcpy((void *)DS_Y_MEM, from, DS_MAX_BITS / 8); fence(); from += DS_MAX_BITS / 8;
    memcpy((void *)DS_M_MEM, from, DS_MAX_BITS / 8); fence(); from += DS_MAX_BITS / 8;
    memcpy((void *)DS_RB_MEM, from, DS_MAX_BITS / 8); fence(); from += DS_MAX_BITS / 8;
    memcpy((void *)DS_BOX_MEM, from, DS_IV_MEM - DS_BOX_MEM); fence();
    for (int i = 0; i < 4; i++) { uint32_t w; memcpy(&w, d->iv + 4 * i, 4); REG(DS_IV_MEM + 4 * i) = w; }
    memcpy((void *)DS_X_MEM, msg, n); fence();
    REG(DS_SET_CONTINUE_REG) = 1;
    SPIN_US(REG(DS_QUERY_BUSY_REG) > 0, 2000000);
    uint32_t chk = REG(DS_QUERY_CHECK_REG);                                   /* 0 ok, 1 MD fail, 2 padding fail, 3 both */
    if (chk == 0 || chk == 2) memcpy(sig, (void *)DS_Z_MEM, n);
    fence();
    REG(DS_SET_FINISH_REG) = 1;
    SPIN_US(REG(DS_QUERY_BUSY_REG) > 0, 100000);
    hidle(); REG(HMAC_SET_INVALIDATE_DS_REG) = 1; REG(HMAC_SET_INVALIDATE_JTAG_REG) = 1;
    if (chk & 1) { memset(sig, 0, n); return -2; }
    if (chk == 2) { memset(sig, 0, n); return -3; }
    return 0;
}
