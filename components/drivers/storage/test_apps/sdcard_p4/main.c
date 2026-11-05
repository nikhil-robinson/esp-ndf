#include "sdcard.h"
#include "console.h"
#include <string.h>

static uint8_t a[4096] __attribute__((aligned(64))), b[4096] __attribute__((aligned(64)));

int main(void)
{
    int err = 0;
    sdcard_cfg_t c = { .width = 4, .freq_khz = 50000 };
    if (sdcard_init(&c)) { printf("sdcard test: no card, errors 1\n"); return 1; }
    printf("sdcard: %lu sectors (%lu MB)\n", (unsigned long)sdcard_sectors(), (unsigned long)(sdcard_sectors() / 2048));
    uint32_t lba = sdcard_sectors() - 64;                              /* the last sectors: away from a file system */
    for (int i = 0; i < 4096; i++) a[i] = (uint8_t)(i * 31 + 7);
    err += sdcard_write(lba, a, 8) != 0;
    err += sdcard_read(lba, b, 8) != 0;
    err += memcmp(a, b, 4096) != 0;
    printf("sdcard test errors %d\n", err);
    return err != 0;
}
