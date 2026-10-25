/* Host fuzz of drivers/usb/usb_msc.c (the USB host is untrusted): random and boundary CBWs (LBA near 2^32, counts,
 * lengths, directions, opcodes) into command handling over an ASan-guarded RAM disk; the USB layer is stubbed
 * (OUT data = random bytes, IN data discarded). Any access outside the disk is an ASan error.
 *   gcc -g -O1 -fsanitize=address,undefined -Iinclude -Isoc/esp32s3 tools/fuzz/fuzz_msc.c drivers/usb/usb_msc.c -o /tmp/fm */
#include "usb.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NB 64
static uint8_t *ram;
#ifdef NO_BDEV_CHECK                                    /* control: a block device trusting the driver */
#define bdev_range_ok(b, s, n) ((void)(b), 1)
#endif
static int rd(bdev_t *b, uint32_t s, void *p, uint32_t n) { if (!bdev_range_ok(b, s, n)) return -1; memcpy(p, ram + s * 512, n * 512); return 0; }
static int wr(bdev_t *b, uint32_t s, const void *p, uint32_t n) { if (!bdev_range_ok(b, s, n)) return -1; memcpy(ram + s * 512, p, n * 512); return 0; }
static bdev_t disk = { 512, NB, rd, wr, 0, 0 };

static uint8_t in_q[64]; static size_t in_n, in_pos; static long out_budget;   /* host -> device bytes */
bool usb_cdc_ready(void) { return out_budget > 0; }
void usb_dev_poll(void) { out_budget--; }
int usb_msc_pipe(void) { return 0; }
size_t usb_cdc_write(const void *b, size_t n) { (void)b; return n; }
size_t usb_cdc_read(void *b, size_t n) { (void)b; (void)n; return 0; }
size_t usb_pipe_write(int p, const void *b, size_t n) { (void)p; (void)b; out_budget--; return n; }
size_t usb_pipe_read(int p, void *b, size_t max)
{
    (void)p;
    if (in_pos < in_n) { size_t k = in_n - in_pos < max ? in_n - in_pos : max; memcpy(b, in_q + in_pos, k); in_pos += k; return k; }
    size_t k = max < 512 ? max : 512; for (size_t i = 0; i < k; i++) ((uint8_t *)b)[i] = rand(); out_budget--; return k;
}

static uint32_t pick32(void)
{
    static const uint32_t edge[] = { 0, 1, NB - 1, NB, NB + 1, 0x7FFFFFFF, 0x80000000u, 0xFFFFFFFFu, 0xFFFFFFFEu, 0xFFFFFF00u };
    return rand() % 2 ? edge[rand() % 10] : (uint32_t)rand();
}

int main(int argc, char **argv)
{
    long rounds = argc > 1 ? atol(argv[1]) : 200000;
    srand(argc > 2 ? atoi(argv[2]) : 1);
    ram = malloc(NB * 512);
    usb_msc_attach(&disk, false);
    static const uint8_t ops[] = { 0x00, 0x03, 0x12, 0x1A, 0x1B, 0x1E, 0x23, 0x25, 0x28, 0x2A, 0x2F, 0x35, 0xA1, 0x5A, 0xFF };
    for (long r = 0; r < rounds; r++) {
        uint8_t c[31] = { 'U', 'S', 'B', 'C' };
        uint32_t len = rand() % 3 ? pick32() % 70000 : pick32();
        memcpy(c + 8, &len, 4); c[12] = rand() % 2 ? 0x80 : 0; c[14] = 10;
        uint8_t *cb = c + 15; cb[0] = ops[rand() % sizeof ops];
        uint32_t lba = pick32(); cb[2] = lba >> 24; cb[3] = lba >> 16; cb[4] = lba >> 8; cb[5] = lba;
        uint16_t cnt = rand() % 2 ? (uint16_t)(len / 512) : (uint16_t)rand(); cb[7] = cnt >> 8; cb[8] = cnt;
        if (rand() % 8 == 0) for (int i = 4; i < 31; i++) c[i] = rand();
        memcpy(in_q, c, 31); in_n = 31; in_pos = 0; out_budget = 400;
        usb_msc_reset();
        usb_msc_poll();
    }
    usb_msc_stats_t s; usb_msc_stats(&s);
    printf("fuzz_msc: %ld CBWs, %lu commands, %lu errors answered, no memory error\n", rounds, (unsigned long)s.cmds, (unsigned long)s.errors);
    return 0;
}
