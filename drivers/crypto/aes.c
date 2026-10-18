/* AES accelerator, typical (CPU-fed) mode: one 16 B block per trigger,
 * ~15 cycles of crypto work; the register traffic dominates. */
#include "aes.h"
#include "clk.h"
#include <string.h>

#define AES_BASE    0x6003A000
#define AES_KEY(i)  (AES_BASE + 0x00 + 4 * (i))
#define AES_IN(i)   (AES_BASE + 0x20 + 4 * (i))
#define AES_OUT(i)  (AES_BASE + 0x30 + 4 * (i))
#define AES_MODE    (AES_BASE + 0x40)     /* 0 enc128, 2 enc256, 4 dec128, 6 dec256 */
#define AES_TRIGGER (AES_BASE + 0x48)
#define AES_STATE   (AES_BASE + 0x4C)     /* 0 idle */
#define AES_DMA_EN  (AES_BASE + 0x90)

static const uint8_t *cur_key;
static int cur_mode = -1;

void aes_init(void)
{
    periph_enable(PERIPH_AES);
    periph_enable(PERIPH_DS);             /* DS reset also holds AES in reset */
    REG(AES_DMA_EN) = 0;
    cur_key = NULL; cur_mode = -1;
}

static uint32_t ld(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

void aes_block(const uint8_t *key, int key_len, bool encrypt, const uint8_t in[16], uint8_t out[16])
{
    int mode = (key_len == 32 ? 2 : 0) | (encrypt ? 0 : 4);
    if (key != cur_key || mode != cur_mode) {          /* key regs survive between blocks */
        for (int i = 0; i < key_len / 4; i++) REG(AES_KEY(i)) = ld(key + 4 * i);
        REG(AES_MODE) = mode;
        cur_key = key; cur_mode = mode;
    }
    for (int i = 0; i < 4; i++) REG(AES_IN(i)) = ld(in + 4 * i);
    REG(AES_TRIGGER) = 1;
    while (READ_ONCE(REG(AES_STATE))) ;
    for (int i = 0; i < 4; i++) { uint32_t w = REG(AES_OUT(i)); memcpy(out + 4 * i, &w, 4); }
}

void aes_xts(const uint8_t key[64], uint32_t tweak, bool encrypt, const void *in, void *out, size_t n)
{
    uint8_t t[16] = { 0 }, x[16];
    memcpy(t, &tweak, 4);
    aes_block(key + 32, 32, true, t, t);
    const uint8_t *s = in;
    uint8_t *d = out;
    for (size_t j = 0; j < n; j += 16) {
        for (int i = 0; i < 16; i++) x[i] = s[j + i] ^ t[i];
        aes_block(key, 32, encrypt, x, x);
        for (int i = 0; i < 16; i++) d[j + i] = x[i] ^ t[i];
        uint8_t c = t[15] >> 7;                         /* t *= alpha in GF(2^128), little-endian */
        for (int i = 15; i > 0; i--) t[i] = t[i] << 1 | t[i - 1] >> 7;
        t[0] = t[0] << 1 ^ (c ? 0x87 : 0);
    }
}
