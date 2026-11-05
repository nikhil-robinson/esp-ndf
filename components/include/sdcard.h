/* SD / SDHC / SDXC memory card on the SD/MMC host controller (ESP32-P4 only, TRM chapter 57), slot 0 on its dedicated pins
 * (GPIO39 D0, 40 D1, 41 D2, 42 D3, 43 CLK, 44 CMD; pull-ups on the board), 1 or 4 data bits, default speed 25 MHz or high
 * speed 50 MHz, internal DMA. The card is a block device (bdev.h) with 512-byte sectors, ready for FAT. The card power
 * switch / LDO channel 4 (3.3 V) is the board's: NDF_SD_LDO_CHAN (default 4, 0: none) switches the on-chip LDO on. */
#pragma once
#include "ndf.h"
#include "bdev.h"

typedef struct { int width; int freq_khz; } sdcard_cfg_t;      /* width 1 or 4; freq_khz 400 .. 50000 (25000 default speed, 50000 high speed) */

int sdcard_init(const sdcard_cfg_t *c);                         /* power, clock, identification, bus width, speed; 0 or -1 */
uint32_t sdcard_sectors(void);
int sdcard_read(uint32_t lba, void *buf, uint32_t n);          /* buf 4-byte aligned */
int sdcard_write(uint32_t lba, const void *buf, uint32_t n);
int bdev_sdcard(bdev_t *b);                                     /* the initialised card as a block device */
