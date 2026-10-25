/* Host fuzz of drivers/storage/wl.c: corrupted and CRC-valid-but-crafted wear-levelling states (positions, move
 * counts, sizes) on a simulated NOR partition; mount must refuse or map every sector inside the data area, and
 * reads/writes through the block device must never fault (ASan/UBSan).
 *   gcc -g -O1 -fsanitize=address,undefined -Iinclude -Isoc/esp32s3 tools/fuzz/fuzz_wl.c drivers/storage/wl.c lib/crc32.c */
#include "wl.h"
#include "crc32.h"
#undef REG
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FSZ (64 * 4096)
static uint8_t nor[FSZ], img[FSZ];
static partition_t P = { .type = PART_DATA, .subtype = PART_SUB_FAT, .offset = 0, .size = FSZ, .label = "w" };
int partition_read(const partition_t *p, uint32_t o, void *b, size_t n) { (void)p; if (o > FSZ || n > FSZ - o) return -1; memcpy(b, nor + o, n); return 0; }
int partition_write(const partition_t *p, uint32_t o, const void *b, size_t n) { (void)p; if (o > FSZ || n > FSZ - o) return -1; for (size_t i = 0; i < n; i++) nor[o + i] &= ((const uint8_t *)b)[i]; return 0; }
int partition_erase(const partition_t *p, uint32_t o, size_t n) { (void)p; if (o % 4096 || n % 4096 || o > FSZ || n > FSZ - o) return -1; memset(nor + o, 0xFF, n); return 0; }

uint32_t rng_u32(void) { return 0x12345678u; }

int main(int argc, char **argv)
{
    long rounds = argc > 1 ? atol(argv[1]) : 20000; srand(argc > 2 ? atoi(argv[2]) : 1);
    memset(nor, 0xFF, FSZ);
    wl_t w; bdev_t b; static uint8_t sec[4096];
    if (wl_mount(&w, &b, &P)) { puts("mount failed"); return 1; }
    for (uint32_t s = 0; s < b.sector_count; s++) { memset(sec, s, 4096); b.write(&b, s, sec, 1); }
    uint32_t st1 = w.addr_state1, st2 = w.addr_state2;
    memcpy(img, nor, FSZ);
    long refused = 0;
    for (long r = 0; r < rounds; r++) {
        memcpy(nor, img, FSZ);
        wl_state_t s; memcpy(&s, nor + st1, sizeof s);
        uint32_t *f = (uint32_t *)&s;
        static const uint32_t edge[] = { 0, 1, 2, 62, 63, 64, 65, 0x7FFFFFFF, 0xFFFFFFFF, 0x80000000u, 4096, 4095 };
        for (int k = 0; k < 1 + rand() % 4; k++) f[rand() % 15] = rand() % 2 ? edge[rand() % 12] : (uint32_t)rand();
        if (rand() % 4) s.crc32 = crc32_le(0xFFFFFFFF, &s, 60);              /* mostly CRC-valid: past the check */
        memcpy(nor + st1, &s, sizeof s);
        if (rand() % 2) memcpy(nor + st2, &s, sizeof s);
        for (int k = 0; k < rand() % 8; k++) nor[st1 + 64 + rand() % 4000] = rand();   /* position records */
        wl_t w2; bdev_t b2;
        if (wl_mount(&w2, &b2, &P)) { refused++; continue; }
        for (int k = 0; k < 8; k++) { uint32_t sx = rand() % (b2.sector_count + 3); b2.read(&b2, sx, sec, 1); if (k & 1) b2.write(&b2, sx, sec, 1); }
    }
    printf("fuzz_wl: %ld rounds (%ld refused), no memory error\n", rounds, refused);
    return 0;
}
