/* AES accelerator, typical (CPU-fed) mode: one 16 B block per trigger,
 * ~15 cycles of crypto work; the register traffic dominates. */
#include "aes.h"
#include "clk.h"
#include "gdma.h"   /* DMA modes: chips with SOC_CRYPTO_NO_DMA (P4 so far) use the CPU path only */
#include <string.h>
#include <strings.h>
#include "irq.h"
#include "cpu.h"
#if SOC_CPU_RISCV
#include "cache_p4.h"
#endif

#ifndef SOC_AES_BASE
#define SOC_AES_BASE 0x6003A000        /* ESP32-S3; other chips set it in soc_caps.h */
#endif
#define AES_BASE    SOC_AES_BASE
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

/* The engine is one per chip: callers on both cores and in interrupts are serialised (spinlock + interrupts off on
 * this core); nested calls on the holding core (aes_xts -> aes_block, GCM) pass through. After every operation the key
 * registers are cleared, so no key stays readable in the engine. Every hardware wait is bounded. */
static volatile uint32_t lk; static volatile int owner = -1, depth;
uint32_t aes_lock(void)
{
    uint32_t st = irq_lock();
    if (owner == core_id()) { depth++; return st; }
    while (__atomic_exchange_n(&lk, 1, __ATOMIC_ACQUIRE)) {}
    owner = core_id(); depth = 1;
    return st;
}
void aes_unlock(uint32_t st)
{
    if (--depth == 0) {
        for (int i = 0; i < 8; i++) REG(AES_KEY(i)) = 0;
        owner = -1;
        __atomic_store_n(&lk, 0, __ATOMIC_RELEASE);
    }
    irq_unlock(st);
}
static bool wait_until(volatile uint32_t *r, uint32_t want_mask, uint32_t want)   /* bounded: 10 ms */
{
    for (uint32_t t = ccount(); (*r & want_mask) != want; ) if (ccount() - t > 2400000u) return false;
    return true;
}

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
    uint32_t st = aes_lock();
    for (int i = 0; i < key_len / 4; i++) REG(AES_KEY(i)) = ld(key + 4 * i);   /* every block: a cache by pointer missed reused buffers */
    REG(AES_MODE) = mode;
    for (int i = 0; i < 4; i++) REG(AES_IN(i)) = ld(in + 4 * i);
    REG(AES_TRIGGER) = 1;
    if (!wait_until(&REG(AES_STATE), ~0u, 0)) memset(out, 0, 16);              /* engine hung: no stale output */
    else for (int i = 0; i < 4; i++) { uint32_t w = REG(AES_OUT(i)); memcpy(out + 4 * i, &w, 4); }
    aes_unlock(st);
}

void aes_xts(const uint8_t key[64], uint32_t tweak, bool encrypt, const void *in, void *out, size_t n)
{
    uint8_t t[16] = { 0 }, x[16];
    uint32_t st = aes_lock();
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
    explicit_bzero(t, sizeof t); explicit_bzero(x, sizeof x);
    aes_unlock(st);
}

#if !defined(SOC_CRYPTO_NO_DMA) || SOC_AES_DMA
/* ---- block modes by DMA (ESP-IDF port/aes/dma/esp_aes_dma_core.c: GDMA OUT -> AES -> GDMA IN, BLOCK_NUM blocks,
 * wait for the IN EOF and state DONE, IV read back for the next call, DMA_EXIT) ---- */
#define DESCS 16                           /* 16 x 4 KB per call, larger requests are split */
static int aes_ch = -1;

#if SOC_CPU_RISCV
#define DESC_BLOCKS (GDMA_DESC_MAX / 16)          /* blocks per descriptor */
#define DESC_ALIGN  __attribute__((aligned(64)))  /* descriptors never share a cache line with other data */
#define DMA_GRAIN   64                            /* the AXI DMA moves whole 64-byte lines */
static bool dma_ok(const void *p, size_t n) { return (uintptr_t)p >= SOC_VEC_RAM_LO && (uintptr_t)p + n <= SOC_VEC_RAM_HI && !((uintptr_t)p & 3); }
#else
#define DESC_BLOCKS 255
#define DESC_ALIGN
#define DMA_GRAIN   16
static bool dma_ok(const void *p, size_t n) { return (uintptr_t)p >= 0x3FC88000u && (uintptr_t)p + n <= 0x3FD00000u && !((uintptr_t)p & 3); }
#endif

static void dma_run(int mode, const uint8_t *key, int key_len, bool enc, uint8_t iv[16], const uint8_t *in, uint8_t *out, size_t nb)
{
    static gdma_desc_t din[DESCS] DESC_ALIGN, dout[DESCS] DESC_ALIGN;
    for (int i = 0; i < key_len / 4; i++) REG(AES_KEY(i)) = ld(key + 4 * i);
    REG(AES_MODE) = (key_len == 32 ? 2 : 0) | (enc ? 0 : 4);
    REG(AES_BMODE) = (uint32_t)mode;
    if (mode == AES_CTR) REG(AES_INC) = 0;
    if (mode != AES_ECB) for (int i = 0; i < 4; i++) REG(AES_IV(i)) = ld(iv + 4 * i);
    REG(AES_DMA_EN) = 1;
    gdma_connect(aes_ch, GDMA_AES);
#if SOC_CPU_RISCV
    cache_dma_wbinv(in, nb * 16); cache_dma_wbinv(out, nb * 16);        /* source in memory; no dirty destination line written back over the result */
#endif
    gdma_chain(dout, DESCS, (void *)in, nb * 16, true);
    gdma_chain(din, DESCS, out, nb * 16, true);
    gdma_in_start(aes_ch, din);
    gdma_out_start(aes_ch, dout);
    REG(AES_BNUM) = nb;
    REG(AES_TRIGGER) = 1;
    for (uint32_t t = ccount(); !gdma_in_eof(aes_ch) && ccount() - t < 240000000u; ) {}   /* bounded: 1 s */
    wait_until(&REG(AES_STATE), ~0u, 2);
#if SOC_CPU_RISCV
    cache_dma_wbinv(out, nb * 16);
#endif
    if (mode != AES_ECB) for (int i = 0; i < 4; i++) { uint32_t w = REG(AES_IV(i)); memcpy(iv + 4 * i, &w, 4); }
    REG(AES_DEXIT) = 0;
    REG(AES_DMA_EN) = 0;
}

#if SOC_AES_GCM_HW
/* ---- GCM mode (ESP-IDF esp_aes_gcm_crypt_and_tag + esp_aes_process_dma_gcm): the engine derives H, runs CTR and GHASH
 * over aad, data and the length block, and leaves the tag in T0. DMA in: aad (zero padded to blocks), data (the last
 * partial block padded), 16 bytes of bit lengths; DMA out: the data. ---- */
#define AES_H_MEM   (AES_BASE + 0x60)
#define AES_J0_MEM  (AES_BASE + 0x70)
#define AES_T0_MEM  (AES_BASE + 0x80)
#define AES_AADN    (AES_BASE + 0xA0)
#define AES_REMB    (AES_BASE + 0xA4)
#define AES_CONT    (AES_BASE + 0xA8)
#define GCM_AAD_MAX 1024
#define GCM_LEN_MAX (DESCS * 4032)
static void chain_app(gdma_desc_t **tail, gdma_desc_t *piece, int n) { if (*tail) (*tail)->next = piece; *tail = &piece[n - 1]; }

int aes_gcm_hw(const uint8_t *key, int key_len, bool encrypt, const uint8_t iv[12], const void *aad, size_t aad_len,
               const void *in, void *out, size_t len, uint8_t tag[16])
{
    static gdma_desc_t da[1] DESC_ALIGN, dd[DESCS] DESC_ALIGN, dt[1] DESC_ALIGN, dl[1] DESC_ALIGN, ddo[DESCS] DESC_ALIGN, dto[1] DESC_ALIGN;
    static uint8_t abuf[GCM_AAD_MAX] __attribute__((aligned(64))), tbuf[64] __attribute__((aligned(64))), lbuf[64] __attribute__((aligned(64))), obuf[64] __attribute__((aligned(64)));
    if (!len || len > GCM_LEN_MAX || aad_len > GCM_AAD_MAX || !dma_ok(in, len) || !dma_ok(out, len) || (aad_len && !dma_ok(aad, aad_len))) return -2;
    if (!up) aes_init();
    int ch = gdma_alloc();
    if (ch < 0) return -2;
    size_t full = len & ~(size_t)15, rem = len & 15, ab = (aad_len + 15) / 16, blocks = full / 16 + (rem ? 1 : 0);
    memset(abuf, 0, ab * 16); memcpy(abuf, aad, aad_len);
    memset(tbuf, 0, 16); memcpy(tbuf, (const uint8_t *)in + full, rem);
    memset(lbuf, 0, 16);                                                          /* [len(A)]64 || [len(C)]64, big endian bits */
    uint64_t abits = (uint64_t)aad_len * 8, cbits = (uint64_t)len * 8;
    for (int i = 0; i < 8; i++) { lbuf[7 - i] = (uint8_t)(abits >> (8 * i)); lbuf[15 - i] = (uint8_t)(cbits >> (8 * i)); }
    gdma_desc_t *ih = 0, *it = 0, *oh = 0, *ot = 0;
    if (ab) { gdma_chain(da, 1, abuf, ab * 16, false); ih = da; it = &da[0]; }
    if (full) { int n = gdma_chain(dd, DESCS, (void *)in, full, false); if (it) it->next = dd; else ih = dd; it = &dd[n - 1]; }
    if (rem) { gdma_chain(dt, 1, tbuf, 16, false); if (it) it->next = dt; else ih = dt; it = dt; }
    gdma_chain(dl, 1, lbuf, 16, true); it->next = dl;
    if (full) { int n = gdma_chain(ddo, DESCS, out, full, !rem); oh = ddo; ot = &ddo[n - 1]; }
    if (rem) { gdma_chain(dto, 1, obuf, 16, true); if (ot) ot->next = dto; else oh = dto; }
    cache_dma_wbinv(in, len); cache_dma_wbinv(out, len); cache_dma_wbinv(abuf, sizeof abuf); cache_dma_wbinv(tbuf, 64); cache_dma_wbinv(lbuf, 64); cache_dma_wbinv(obuf, 64);
    uint8_t j0[16]; memcpy(j0, iv, 12); j0[12] = j0[13] = j0[14] = 0; j0[15] = 1;
    uint32_t st = aes_lock();
    for (int i = 0; i < key_len / 4; i++) REG(AES_KEY(i)) = ld(key + 4 * i);
    REG(AES_MODE) = (key_len == 32 ? 2 : 0) | (encrypt ? 0 : 4);
    REG(AES_BMODE) = 6;
    REG(AES_DMA_EN) = 1;
    REG(AES_AADN) = (uint32_t)ab;
    REG(AES_REMB) = (uint32_t)((8 * len) % 128);
    REG(AES_TRIGGER) = 1;                                                         /* H = E(0) into H_MEM */
    int r = 0;
    if (!wait_until(&REG(AES_STATE), ~0u, 0)) r = -1;
    for (int i = 0; i < 4; i++) REG(AES_J0_MEM + 4 * i) = ld(j0 + 4 * i);
    gdma_connect(ch, GDMA_AES);
    gdma_in_start(ch, oh);
    gdma_out_start(ch, ih);
    REG(AES_BNUM) = (uint32_t)blocks;
    REG(AES_CONT) = 1;
    for (uint32_t t = ccount(); !gdma_in_eof(ch); ) if (ccount() - t > 240000000u) { r = -1; break; }
    if (!wait_until(&REG(AES_STATE), ~0u, 2)) r = -1;
    for (int i = 0; i < 4; i++) { uint32_t w = REG(AES_T0_MEM + 4 * i); memcpy(tag + 4 * i, &w, 4); }
    REG(AES_DEXIT) = 0;
    REG(AES_DMA_EN) = 0;
    aes_unlock(st);
    gdma_free(ch);
    cache_dma_wbinv(out, len); cache_dma_wbinv(obuf, 64);
    if (rem) memcpy((uint8_t *)out + full, obuf, rem);
    explicit_bzero(abuf, ab * 16); explicit_bzero(tbuf, 16); explicit_bzero(obuf, 16);
    if (r) { memset(out, 0, len); memset(tag, 0, 16); }
    return r;
}
#endif /* SOC_AES_GCM_HW */

#endif /* DMA modes */

static void cpu_mode(int mode, const uint8_t *key, int key_len, bool enc, uint8_t iv[16], const uint8_t *in, uint8_t *out, size_t len)
{
    uint8_t k[16];
    uint32_t st = aes_lock();
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
    explicit_bzero(k, sizeof k);
    aes_unlock(st);
}

int aes_crypt(int mode, const uint8_t *key, int key_len, bool encrypt, uint8_t iv[16], const void *in, void *out, size_t len)
{
    if (!up) aes_init();
    if ((key_len != 16 && key_len != 32) || mode < AES_ECB || mode > AES_CFB128 || mode == 4) return -1;
    if ((mode == AES_ECB || mode == AES_CBC) && (len & 15)) return -1;
    const uint8_t *s = in; uint8_t *d = out;
    size_t full = len & ~(size_t)15;
#if !defined(SOC_CRYPTO_NO_DMA) || SOC_AES_DMA
    full &= ~(size_t)(DMA_GRAIN - 1);
    if (full >= 16u * NDF_AES_DMA_MIN && dma_ok(s, full) && dma_ok(d, full) && (aes_ch = gdma_alloc()) >= 0) {   /* per call */
        uint32_t st = aes_lock();
        for (size_t j = 0; j < full; ) {
            size_t nb = (full - j) / 16; if (nb > DESCS * DESC_BLOCKS) nb = DESCS * DESC_BLOCKS;   /* 4080 B per descriptor (P4: 4032) */
            dma_run(mode, key, key_len, encrypt, iv, s + j, d + j, nb);
            j += nb * 16;
        }
        gdma_free(aes_ch); aes_ch = -1;                   /* the chip has 5 channels: none kept idle */
        aes_unlock(st);
        s += full; d += full; len -= full;
    }
#endif
    if (len) cpu_mode(mode, key, key_len, encrypt, iv, s, d, len);     /* short requests, tails, non-DMA memory */
    return 0;
}
