/* SHA accelerator, CPU-fed (TRM 18; ESP-IDF hal/esp32s3/include/hal/sha_ll.h): message words into SHA_TEXT, mode,
 * START for the first block and CONTINUE after, wait for BUSY to clear. The H registers hold the state in digest
 * byte order (IDF copies them to the output as they are). A context writes its saved state back only when another
 * context used the engine in between. Long runs of blocks in internal RAM go by DMA (IDF port/sha/dma/sha.c: GDMA OUT
 * to the SHA peripheral, BLOCK_NUM <= 63 per run, DMA_START / DMA_CONTINUE). */
#include "sha.h"
#include "clk.h"
#include "xtensa.h"
#include "gdma.h"
#include <string.h>

#define SHA_BASE   0x6003B000u
#define SHA_MODE   (SHA_BASE + 0x00)
#define SHA_START  (SHA_BASE + 0x10)
#define SHA_CONT   (SHA_BASE + 0x14)
#define SHA_BLKNUM (SHA_BASE + 0x0C)
#define SHA_BUSY   (SHA_BASE + 0x18)
#define SHA_DSTART (SHA_BASE + 0x1C)
#define SHA_DCONT  (SHA_BASE + 0x20)
#define SHA_H(i)   (SHA_BASE + 0x40 + 4 * (i))
#define SHA_TEXT(i) (SHA_BASE + 0x80 + 4 * (i))

static bool up;
static const sha_ctx_t *owner;          /* whose state is in the engine */
static uint32_t lk;

static unsigned blk(uint8_t t) { return t >= SHA384 ? 128 : 64; }
static unsigned hwords(uint8_t t) { return t >= SHA384 ? 16 : t == SHA1 ? 5 : 8; }
size_t sha_size(sha_type_t t) { static const uint8_t n[] = { 20, 28, 32, 48, 64, 28, 32 }; return n[t]; }

static uint32_t lock(void)
{
    uint32_t st = irq_lock();
    while (__atomic_exchange_n(&lk, 1, __ATOMIC_ACQUIRE)) {}
    if (!up) { periph_enable(PERIPH_SHA); up = true; }
    return st;
}
static void unlock(uint32_t st) { __atomic_store_n(&lk, 0, __ATOMIC_RELEASE); irq_unlock(st); }

static void blocks(sha_ctx_t *c, const uint8_t *p, size_t n)       /* n whole blocks, engine locked */
{
    unsigned w = blk(c->type) / 4, hw = hwords(c->type);
    if (!c->first && owner != c)
        for (unsigned i = 0; i < hw; i++) REG(SHA_H(i)) = c->h[i];
    REG(SHA_MODE) = c->type;
    static int ch = -1;
    if (n >= NDF_SHA_DMA_MIN && (uintptr_t)p >= 0x3FC88000u && (uintptr_t)p + n * w * 4 <= 0x3FD00000u &&
        !((uintptr_t)p & 3) && (ch >= 0 || (ch = gdma_alloc()) >= 0)) {
        static gdma_desc_t d[2];
        gdma_connect(ch, GDMA_SHA);
        while (n) {
            unsigned k = n > 63 ? 63 : n;                             /* BLOCK_NUM is 6 bits */
            gdma_chain(d, 2, (void *)p, k * w * 4, true);
            gdma_out_start(ch, d);
            REG(SHA_BLKNUM) = k;
            REG(c->first ? SHA_DSTART : SHA_DCONT) = 1;
            c->first = false;
            while (REG(SHA_BUSY)) {}
            p += k * w * 4; n -= k;
        }
    }
    for (; n; n--, p += w * 4) {
        while (REG(SHA_BUSY)) {}
        if (!((uintptr_t)p & 3)) for (unsigned i = 0; i < w; i++) REG(SHA_TEXT(i)) = ((const uint32_t *)p)[i];
        else for (unsigned i = 0; i < w; i++) { uint32_t v; memcpy(&v, p + 4 * i, 4); REG(SHA_TEXT(i)) = v; }
        REG(c->first ? SHA_START : SHA_CONT) = 1;
        c->first = false;
    }
    while (REG(SHA_BUSY)) {}
    for (unsigned i = 0; i < hw; i++) c->h[i] = REG(SHA_H(i));
    owner = c;
}

void sha_start(sha_ctx_t *c, sha_type_t type)
{
    c->type = type; c->first = true; c->fill = 0; c->total = 0;
}

void sha_update(sha_ctx_t *c, const void *data, size_t len)
{
    const uint8_t *p = data;
    unsigned b = blk(c->type);
    c->total += len;
    uint32_t st = lock();
    if (c->fill) {
        unsigned n = b - c->fill < len ? b - c->fill : len;
        memcpy(c->buf + c->fill, p, n); c->fill += n; p += n; len -= n;
        if (c->fill == b) { blocks(c, c->buf, 1); c->fill = 0; }
    }
    if (len >= b) { size_t nb = len / b; blocks(c, p, nb); p += nb * b; len -= nb * b; }
    unlock(st);
    if (len) { memcpy(c->buf, p, len); c->fill = len; }
}

void sha_finish(sha_ctx_t *c, uint8_t *digest)
{
    unsigned b = blk(c->type), lenb = b == 128 ? 16 : 8;
    uint64_t bits = c->total * 8;
    c->buf[c->fill++] = 0x80;
    uint32_t st = lock();
    if (c->fill > b - lenb) { memset(c->buf + c->fill, 0, b - c->fill); blocks(c, c->buf, 1); c->fill = 0; }
    memset(c->buf + c->fill, 0, b - c->fill);
    for (int i = 0; i < 8; i++) c->buf[b - 1 - i] = (uint8_t)(bits >> (8 * i));   /* big-endian length */
    blocks(c, c->buf, 1);
    unlock(st);
    memcpy(digest, c->h, sha_size(c->type));
}

void sha(sha_type_t type, const void *data, size_t len, uint8_t *digest)
{
    sha_ctx_t c;
    sha_start(&c, type); sha_update(&c, data, len); sha_finish(&c, digest);
}
