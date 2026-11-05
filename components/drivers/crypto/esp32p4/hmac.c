/* P4 HMAC accelerator (hmac.h). Flow of ESP-IDF v6.1 esp_security/src/esp_hmac.c (esp32p4 branch) and esp_hal_security/hmac_hal.c +
 * esp32p4/hmac_ll.h: the HMAC, SHA and DS clocks on (DS holds SHA in reset otherwise), start, select the eFuse block and the
 * "HMAC upstream" purpose (8), then 64-byte blocks with SHA-256 padding done here (the length field counts the 512-bit inner
 * key block). Registers: generated regs/hmac.h. NOT VERIFIED ON HARDWARE. */
#include "hmac.h"
#include "clk.h"
#include "cpu.h"
#include "spin.h"
#include "soc.h"
#include "regs/hmac.h"
#include "regs/hp_clkrst.h"
#include "regs/p4reg.h"
#include <string.h>

#define PURPOSE_UP 8

static void idle(void) { SPIN_WHILE(REG(HMAC_QUERY_BUSY_REG) != 0, 100000); }
static void block(const uint8_t *b) { uint32_t w[16]; memcpy(w, b, 64); for (int i = 0; i < 16; i++) REG(HMAC_WR_MESSAGE_MEM + 4 * i) = w[i]; REG(HMAC_SET_MESSAGE_ONE_REG) = 1; }

int hmac_sha256_hw(int key_id, const void *msg, size_t n, uint8_t out[32])
{
    if ((unsigned)key_id > 5 || (!msg && n) || !out) return -1;
    const uint8_t *m = msg;
    uint32_t t = irq_lock();
    FSET(HP_SYS_CLKRST_REG_CRYPTO_HMAC_CLK_EN); FSET(HP_SYS_CLKRST_REG_CRYPTO_SHA_CLK_EN); FSET(HP_SYS_CLKRST_REG_CRYPTO_DS_CLK_EN);
    FSET(HP_SYS_CLKRST_REG_RST_EN_HMAC); FCLR(HP_SYS_CLKRST_REG_RST_EN_HMAC); FCLR(HP_SYS_CLKRST_REG_RST_EN_CRYPTO);
    irq_unlock(t);
    idle(); REG(HMAC_SET_START_REG) = 1;
    idle(); REG(HMAC_SET_PARA_PURPOSE_REG) = PURPOSE_UP; REG(HMAC_SET_PARA_KEY_REG) = key_id; REG(HMAC_SET_PARA_FINISH_REG) = 1;
    idle();
    if (REG(HMAC_QUERY_ERROR_REG)) { REG(HMAC_SET_RESULT_FINISH_REG) = 2; return -1; }
    uint8_t b[64];
    uint64_t bits = __builtin_bswap64((uint64_t)n * 8 + 512);
    if (n + 1 + 8 <= 64) {                                                    /* message + padding + length fit one block */
        memcpy(b, m, n); b[n] = 0x80; memset(b + n + 1, 0, 64 - n - 1);
        memcpy(b + 56, &bits, 8);
        idle(); block(b); idle();                                             /* single-block message: SET_MESSAGE_ONE */
        REG(HMAC_ONE_BLOCK_REG) = 1;
    } else {
        size_t full = n / 64;
        for (size_t i = 1; i < full; i++) { idle(); for (int k = 0; k < 16; k++) { uint32_t w; memcpy(&w, m + 4 * k, 4); REG(HMAC_WR_MESSAGE_MEM + 4 * k) = w; } m += 64; idle(); REG(HMAC_SET_MESSAGE_ING_REG) = 1; }
        if (full) { idle(); for (int k = 0; k < 16; k++) { uint32_t w; memcpy(&w, m + 4 * k, 4); REG(HMAC_WR_MESSAGE_MEM + 4 * k) = w; } m += 64; }
        size_t rem = n % 64;
        if (rem >= 56) {                                                      /* the length does not fit: one more padded block, then the length block */
            memcpy(b, m, rem); b[rem] = 0x80; memset(b + rem + 1, 0, 64 - rem - 1);
            idle(); REG(HMAC_SET_MESSAGE_ING_REG) = 1;
            idle(); for (int k = 0; k < 16; k++) { uint32_t w; memcpy(&w, b + 4 * k, 4); REG(HMAC_WR_MESSAGE_MEM + 4 * k) = w; }
            memset(b, 0, 64);
        } else { memcpy(b, m, rem); b[rem] = 0x80; memset(b + rem + 1, 0, 64 - rem - 1); }
        memcpy(b + 56, &bits, 8);
        idle(); REG(HMAC_SET_MESSAGE_PAD_REG) = 1;
        idle(); for (int k = 0; k < 16; k++) { uint32_t w; memcpy(&w, b + 4 * k, 4); REG(HMAC_WR_MESSAGE_MEM + 4 * k) = w; }
    }
    idle();
    for (int i = 0; i < 8; i++) { uint32_t w = REG(HMAC_RD_RESULT_MEM + 4 * i); memcpy(out + 4 * i, &w, 4); }
    REG(HMAC_SET_RESULT_FINISH_REG) = 2;
    return 0;
}
