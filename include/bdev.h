/* Block device: fixed-size sectors. Filesystems (FAT) sit on top; flash
 * partitions, wear-levelled flash and SD cards implement it. */
#pragma once
#include "ndf.h"
#include "partition.h"

typedef struct bdev {
    uint32_t sector_size, sector_count;
    int (*read)(struct bdev *, uint32_t sector, void *buf, uint32_t count);
    int (*write)(struct bdev *, uint32_t sector, const void *buf, uint32_t count);
    int (*sync)(struct bdev *);
    void *ctx;
} bdev_t;

/* Raw flash partition as 4 KB sectors (erase + program per write; no wear levelling). */
int bdev_partition(bdev_t *b, const partition_t *p);
