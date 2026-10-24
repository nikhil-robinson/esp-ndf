/* AES accelerator, typical (CPU-fed) mode: one 16 B block per trigger,
 * ~15 cycles of crypto work; the register traffic dominates. */
#include "aes.h"
#include "clk.h"
#include "gdma.h"
#include <string.h>

#define AES_BASE    0x6003A000
#define AES_KEY(i)  (AES_BASE + 0x00 + 4 * (i))
#define AES_IN(i)   (AES_BASE + 0x20 + 4 * (i))
#define AES_OUT(i)  (AES_BASE + 0x30 + 4 * (i))
#define AES_MODE    (AES_BASE + 0x40)     /* 0 enc128, 2 enc256, 4 dec128, 6 dec256 */
#define AES_TRIGGER (AES_BASE + 0x48)
#define AES_STATE   (AES_BASE + 0x4C)     /* 0 idle */
#define AES_DMA_EN  (AES_BASE + 0x90)
#define AES_IV(i)   (AES_BASE + 0x50 + 4 * (i))
#define AES_BMODE   (AES_BASE + 0x94)     /* 0 ECB 1 CBC 2 OFB 3 CTR 4 CFB8 5 CFB128 */
#define AES_BNUM    (AES_BASE + 0x98)
#define AES_INC     (AES_BASE + 0x9C)     /* 0: 32-bit counter (CTR) */
#define AES_DEXIT   (AES_BASE + 0xB8)

static bool up;

void aes_init(void)
{
    periph_enable(PERIPH_AES);
    periph_enable(PERIPH_DS);             /* DS reset also holds AES in reset */
    REG(AES_DMA_EN) = 0;
    up = true;
}

static uint32_t ld(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

void aes_block(const uint8_t *key, int key_len, bool encrypt, const uint8_t in[16], uint8_t out[16])
{
    int mode = (key_len == 32 ? 2 : 0) | (encrypt ? 0 : 4);
    if (!up) aes_init();
    for (int i = 0; i < key_len / 4; i++) REG(AES_KEY(i)) = ld(key + 4 * i);   /* every block: a cache by pointer missed reused buffers */
    REG(AES_MODE) = mode;
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

/* ---- block modes by DMA (ESP-IDF port/aes/dma/esp_aes_dma_core.c: GDMA OUT -> AES -> GDMA IN, BLOCK_NUM blocks,
 * wait for the IN EOF and state DONE, IV read back for the next call, DMA_EXIT) ---- */
#define DESCS 16                           /* 16 x 4 KB per call, larger requests are split */
static int aes_ch = -1;

static bool dma_ok(const void *p, size_t n) { return (uintptr_t)p >= 0x3FC88000u && (uintptr_t)p + n <= 0x3FD00000u && !((uintptr_t)p & 3); }

static void dma_run(int mode, const uint8_t *key, int key_len, bool enc, uint8_t iv[16], const uint8_t *in, uint8_t *out, size_t nb)
{
    static gdma_desc_t din[DESCS], dout[DESCS];
    for (int i = 0; i < key_len / 4; i++) REG(AES_KEY(i)) = ld(key + 4 * i);
    REG(AES_MODE) = (key_len == 32 ? 2 : 0) | (enc ? 0 : 4);
    REG(AES_BMODE) = (uint32_t)mode;
    if (mode == AES_CTR) REG(AES_INC) = 0;
    if (mode != AES_ECB) for (int i = 0; i < 4; i++) REG(AES_IV(i)) = ld(iv + 4 * i);
    REG(AES_DMA_EN) = 1;
    gdma_connect(aes_ch, GDMA_AES);
    gdma_chain(dout, DESCS, (void *)in, nb * 16, true);
    gdma_chain(din, DESCS, out, nb * 16, true);
    gdma_in_start(aes_ch, din);
    gdma_out_start(aes_ch, dout);
    REG(AES_BNUM) = nb;
    REG(AES_TRIGGER) = 1;
    while (!gdma_in_eof(aes_ch)) {}
    while (REG(AES_STATE) != 2) {}
    if (mode != AES_ECB) for (int i = 0; i < 4; i++) { uint32_t w = REG(AES_IV(i)); memcpy(iv + 4 * i, &w, 4); }
    REG(AES_DEXIT) = 0;
    REG(AES_DMA_EN) = 0;
}

static void cpu_mode(int mode, const uint8_t *key, int key_len, bool enc, uint8_t iv[16], const uint8_t *in, uint8_t *out, size_t len)
{
    uint8_t k[16];
    for (size_t j = 0; j < len; j += 16) {
        size_t n = len - j < 16 ? len - j : 16;
        switch (mode) {
        case AES_ECB: aes_block(key, key_len, enc, in + j, out + j); break;
        case AES_CBC:
            if (enc) { for (int i = 0; i < 16; i++) k[i] = in[j + i] ^ iv[i]; aes_block(key, key_len, true, k, out + j); memcpy(iv, out + j, 16); }
            else { uint8_t c[16]; memcpy(c, in + j, 16); aes_block(key, key_len, false, c, k); for (int i = 0; i < 16; i++) out[j + i] = k[i] ^ iv[i]; memcpy(iv, c, 16); }
            break;
        case AES_CTR:
            aes_block(key, key_len, true, iv, k);
            for (size_t i = 0; i < n; i++) out[j + i] = in[j + i] ^ k[i];
            for (int i = 15; i >= 12 && !++iv[i]; i--) {}           /* 32-bit big-endian counter, as the engine */
            break;
        case AES_OFB:
            aes_block(key, key_len, true, iv, iv);
            for (size_t i = 0; i < n; i++) out[j + i] = in[j + i] ^ iv[i];
            break;
        case AES_CFB128:
            aes_block(key, key_len, true, iv, k);
            for (size_t i = 0; i < n; i++) { uint8_t c = enc ? (out[j + i] = in[j + i] ^ k[i]) : in[j + i]; if (!enc) out[j + i] = in[j + i] ^ k[i]; iv[i] = c; }
            break;
        }
    }
}

int aes_crypt(int mode, const uint8_t *key, int key_len, bool encrypt, uint8_t iv[16], const void *in, void *out, size_t len)
{
    if (!up) aes_init();
    if ((key_len != 16 && key_len != 32) || mode < AES_ECB || mode > AES_CFB128 || mode == 4) return -1;
    if ((mode == AES_ECB || mode == AES_CBC) && (len & 15)) return -1;
    const uint8_t *s = in; uint8_t *d = out;
    size_t full = len & ~(size_t)15;
    if (full >= 16u * NDF_AES_DMA_MIN && dma_ok(s, full) && dma_ok(d, full) && (aes_ch >= 0 || (aes_ch = gdma_alloc()) >= 0)) {
        for (size_t j = 0; j < full; ) {
            size_t nb = (full - j) / 16; if (nb > DESCS * 255) nb = DESCS * 255;   /* 4080 B per descriptor */
            dma_run(mode, key, key_len, encrypt, iv, s + j, d + j, nb);
            j += nb * 16;
        }
        s += full; d += full; len -= full;
    }
    if (len) cpu_mode(mode, key, key_len, encrypt, iv, s, d, len);     /* short requests, tails, non-DMA memory */
    return 0;
}
