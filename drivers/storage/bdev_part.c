/* Raw partition block device: 1 sector = 1 flash erase unit (4 KB). */
#include "bdev.h"
#include "flash.h"

static int rd(bdev_t *b, uint32_t s, void *buf, uint32_t n)
{
    if (!bdev_range_ok(b, s, n)) return -1;
    return partition_read(b->ctx, s * FLASH_SECTOR, buf, n * FLASH_SECTOR);
}

static int wr(bdev_t *b, uint32_t s, const void *buf, uint32_t n)
{
    const uint8_t *p = buf;
    if (!bdev_range_ok(b, s, n)) return -1;
    for (uint32_t i = 0; i < n; i++, p += FLASH_SECTOR) {
        if (partition_erase(b->ctx, (s + i) * FLASH_SECTOR, FLASH_SECTOR)) return -1;
        if (partition_write(b->ctx, (s + i) * FLASH_SECTOR, p, FLASH_SECTOR)) return -1;
    }
    return 0;
}

static int sync_(bdev_t *b) { (void)b; return 0; }

int bdev_partition(bdev_t *b, const partition_t *p)
{
    if (!p) return -1;
    *b = (bdev_t){ FLASH_SECTOR, p->size / FLASH_SECTOR, rd, wr, sync_, (void *)p };
    return 0;
}
