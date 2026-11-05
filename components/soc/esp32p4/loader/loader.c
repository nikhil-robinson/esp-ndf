/* P4 loader (BOOT_FAST=1): the ROM loads only this (a few KB) from flash 0x2000 into the top of the ROM-loadable L2MEM window
 * (loader.ld), then it does what ESP-IDF's 2nd-stage bootloader does for a RAM-only app (bootloader_utility.c
 * unpack_load_app): chooses the image (factory at NDF_BOOT_LOAD_ADDR, or an OTA slot by the ESP-IDF otadata select entries),
 * checks the header, the segment ranges and the appended SHA-256, copies the segments to RAM and jumps to the entry.
 * Flash is read, erased and written with the ROM's own routines (esp32p4.rom.ld esp_rom_spiflash_*): the ROM has set the
 * chip up. OTA rules are the S3 loader's (components/soc/esp32s3/loader): highest valid select entry -> slot
 * (seq - 1) % slots; NEW -> checked once, with NDF_OTA_ROLLBACK PENDING_VERIFY -> ABORTED if the app never confirmed it.
 * The choice is left in the note at SOC_OTA_NOTE ("OTAS", image offset, XIP offset) for ota.c / cache.c. NOT RUN ON
 * HARDWARE (docs/P4.md). */
#include <stdint.h>
#include <stdbool.h>
#include "ndf.h"

#define ROM_FLASH_ERASE_SECTOR(n)   ((int (*)(uint32_t))0x4fc0014c)(n)
#define ROM_FLASH_WRITE(a, p, n)    ((int (*)(uint32_t, const uint32_t *, int32_t))0x4fc00154)((a), (p), (n))
#define ROM_FLASH_READ(a, p, n)     ((int (*)(uint32_t, uint32_t *, int32_t))0x4fc00158)((a), (p), (n))
#define ROM_FLASH_UNLOCK()          ((int (*)(void))0x4fc0015c)()
#define ROM_WRITEBACK_ALL(m)        ((int (*)(uint32_t))0x4fc00414)(m)
#define ROM_INVALIDATE_ALL(m)       ((int (*)(uint32_t))0x4fc00404)(m)
#define APP_LO   0x4FF80000u
#define APP_HI   SOC_LOADER_BASE
#define IMG_MAX  0x200000u

static int rd(uint32_t off, void *dst, uint32_t n) { return ROM_FLASH_READ(off, (uint32_t *)dst, (int32_t)n); }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

/* ---- SHA-256 (the esptool-appended digest of the whole image) ---- */
static const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2 };
#define ROR(x, n) ((x) >> (n) | (x) << (32 - (n)))
static void sha_block(uint32_t h[8], const uint8_t *b)
{
    uint32_t w[64], a = h[0], bb = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 16; i++) w[i] = (uint32_t)b[4 * i] << 24 | b[4 * i + 1] << 16 | b[4 * i + 2] << 8 | b[4 * i + 3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3), s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = hh + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
        uint32_t t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & bb) ^ (a & c) ^ (bb & c));
        hh = g; g = f; f = e; e = d + t1; d = c; c = bb; bb = a; a = t1 + t2;
    }
    h[0] += a; h[1] += bb; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
}
static void sha_flash(uint32_t off, uint32_t len, uint32_t h[8])
{
    static const uint32_t init[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
    uint8_t blk[64] __attribute__((aligned(4)));
    for (int i = 0; i < 8; i++) h[i] = init[i];
    uint32_t done = 0;
    for (; done + 64 <= len; done += 64) { rd(off + done, blk, 64); sha_block(h, blk); }
    uint32_t rem = len - done;                                                /* len is a multiple of 16: rem is 0, 16, 32 or 48 */
    for (uint32_t i = 0; i < 64; i++) blk[i] = 0;
    if (rem) rd(off + done, blk, rem);
    blk[rem] = 0x80;
    if (rem >= 56) { sha_block(h, blk); for (int i = 0; i < 64; i++) blk[i] = 0; }
    uint32_t lo = len << 3, hi = len >> 29;
    for (int i = 0; i < 4; i++) { blk[63 - i] = (uint8_t)(lo >> (8 * i)); blk[59 - i] = (uint8_t)(hi >> (8 * i)); }
    sha_block(h, blk);
}

/* ---- image check: header, segment ranges (RAM below the loader only), entry, SHA-256 ---- */
static bool seg_ok(uint32_t dst, uint32_t len)
{
    return !(len & 3) && len <= IMG_MAX && dst >= APP_LO && dst < APP_HI && len <= APP_HI - dst;
}
static bool image_ok(uint32_t off, uint32_t *entry_out)
{
    uint8_t hd[24] __attribute__((aligned(4)));
    rd(off, hd, 24);
    if (hd[0] != 0xE9 || hd[1] > 16 || !hd[23]) return false;                 /* SHA-256 appended: required */
    uint32_t entry = rd32(hd + 4), p = off + 24;
    if (entry < APP_LO || entry >= APP_HI) return false;
    for (uint32_t s = 0, n = hd[1]; s < n; s++) {
        uint8_t sh[8] __attribute__((aligned(4)));
        rd(p, sh, 8);
        uint32_t len = rd32(sh + 4);
        if (!seg_ok(rd32(sh), len) || p - off + 8 + len > IMG_MAX) return false;
        p += 8 + len;
    }
    uint32_t len = ((p - off) + 16) & ~15u;                                  /* padding + checksum byte */
    uint32_t h[8], want[8];
    sha_flash(off, len, h);
    rd(off + len, want, 32);
    for (int i = 0; i < 8; i++) if (h[i] != ((want[i] & 0xFF) << 24 | (want[i] & 0xFF00) << 8 | (want[i] >> 8 & 0xFF00) | want[i] >> 24)) return false;
    if (entry_out) *entry_out = entry;
    return true;
}

/* ---- OTA slot choice (ESP-IDF otadata format) ---- */
enum { ST_NEW = 0, ST_PENDING = 1, ST_VALID = 2, ST_INVALID = 3, ST_ABORTED = 4 };
typedef struct { uint32_t seq; uint8_t label[20]; uint32_t state; uint32_t crc; } sel_t;
static uint32_t crc32_le(uint32_t crc, const uint8_t *p, int n)
{
    crc = ~crc;
    while (n--) { crc ^= *p++; for (int k = 0; k < 8; k++) crc = crc >> 1 ^ (0xEDB88320u & -(crc & 1)); }
    return ~crc;
}
static void sel_write(uint32_t at, sel_t *e)
{
    e->crc = crc32_le(0xFFFFFFFFu, (const uint8_t *)&e->seq, 4);
    ROM_FLASH_UNLOCK();
    ROM_FLASH_ERASE_SECTOR(at / 0x1000);
    ROM_FLASH_WRITE(at, (const uint32_t *)e, 32);
}
#if NDF_OTA
static uint32_t choose_image(uint32_t *xip, bool *pending)
{
    uint8_t pt[32] __attribute__((aligned(4)));
    uint32_t factory = NDF_BOOT_LOAD_ADDR, otad = 0, slot[16], ns = 0;
    *pending = false;
    for (int i = 0; i < 95; i++) {
        rd(NDF_PART_TABLE_ADDR + 32u * i, pt, 32);
        if (pt[0] != 0xAA || pt[1] != 0x50) break;
        uint32_t o = rd32(pt + 4);
        if (pt[2] == 0 && pt[3] >= 0x10 && pt[3] < 0x20 && ns < 16) slot[ns++] = o;
        else if (pt[2] == 1 && pt[3] == 0) otad = o;
    }
    *xip = NDF_XIP_ADDR;
    if (!otad || !ns) return factory;
    for (;;) {
        sel_t e[2]; int best = -1;
        for (int k = 0; k < 2; k++) {
            rd(otad + 0x1000u * k, &e[k], 32);
            bool ok = e[k].seq != 0xFFFFFFFFu && e[k].state != ST_INVALID && e[k].state != ST_ABORTED &&
                      e[k].crc == crc32_le(0xFFFFFFFFu, (const uint8_t *)&e[k].seq, 4);
            if (ok && (best < 0 || e[k].seq > e[best].seq)) best = k;
        }
        if (best < 0) return factory;
        uint32_t at = otad + 0x1000u * best, img = slot[(e[best].seq - 1) % ns];
        if (NDF_OTA_ROLLBACK && e[best].state == ST_PENDING) { e[best].state = ST_ABORTED; sel_write(at, &e[best]); continue; }
        if (e[best].state == ST_NEW) {
            if (!image_ok(img, 0)) { e[best].state = ST_INVALID; sel_write(at, &e[best]); continue; }
            if (NDF_OTA_ROLLBACK) { e[best].state = ST_PENDING; sel_write(at, &e[best]); *pending = true; }
        }
        uint32_t m; rd(img, &m, 4);
        if ((m & 0xFF) != 0xE9) { e[best].state = ST_INVALID; sel_write(at, &e[best]); continue; }
        *xip = img + NDF_OTA_XIP_OFF;
        return img;
    }
}
#endif

void loader_main(void)
{
    uint32_t img = NDF_BOOT_LOAD_ADDR, xip = NDF_XIP_ADDR, entry;
    bool pending = false;
    (void)pending;
#if NDF_OTA
    img = choose_image(&xip, &pending);
#endif
    volatile uint32_t *note = (volatile uint32_t *)SOC_OTA_NOTE;
    if (img != NDF_BOOT_LOAD_ADDR) { note[0] = 0x5341544Fu; note[1] = img; note[2] = xip; } else note[0] = 0;
    if (!image_ok(img, &entry)) for (;;) {}                                      /* no good image: stay (download mode still works) */
    uint8_t hd[24] __attribute__((aligned(4)));
    rd(img, hd, 24);
    uint32_t p = img + 24;
    for (uint32_t s = 0, n = hd[1]; s < n; s++) {
        uint8_t sh[8] __attribute__((aligned(4)));
        rd(p, sh, 8);
        uint32_t dst = rd32(sh), len = rd32(sh + 4);
        rd(p + 8, (uint32_t *)dst, (int32_t)len);
        p += 8 + len;
    }
    ROM_WRITEBACK_ALL(BIT(4));                                                   /* stores into L2MEM reach memory ... */
    ROM_INVALIDATE_ALL(BIT(0) | BIT(1));                                         /* ... and the instruction caches drop stale lines */
    __asm__ volatile("fence.i" ::: "memory");
    ((void (*)(void))entry)();
}
