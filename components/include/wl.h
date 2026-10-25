/* Wear levelling over a flash partition, on-flash compatible with ESP-IDF's
 * wear_levelling (v2, 4 KB sectors): a volume moves between esp-ndf and
 * esp_vfs_fat_spiflash_mount_rw_wl in both directions. */
#pragma once
#include "bdev.h"

typedef struct {
    uint32_t dummy_pos, max_pos, move_count, erase_count, max_erase_count,
             block_size, version, device_id, reserved[7], crc32;
} wl_state_t;   /* 64 B on flash */

typedef struct {
    const partition_t *part;
    wl_state_t st;
    uint32_t state_size, addr_state1, addr_state2, addr_cfg, flash_size;
} wl_t;

/* Mount (creating the WL layout on a blank/foreign partition) and expose it
 * as a block device of 4 KB sectors. */
int wl_mount(wl_t *w, bdev_t *b, const partition_t *p);
