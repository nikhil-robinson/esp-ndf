/* Partition table (ESP-IDF binary format) at flash 0x80000: esp-ndf images
 * live in 0x0-0x7FFFF. Build with tools/mkpart.py, flash with make partition. */
#pragma once
#include "ndf.h"

typedef struct {
    uint8_t  type, subtype;
    uint32_t offset, size;
    char     label[17];
    uint32_t flags;
} partition_t;

enum { PART_APP = 0x00, PART_DATA = 0x01 };
enum { PART_SUB_NVS = 0x02, PART_SUB_NVS_KEYS = 0x04, PART_SUB_PHY = 0x01, PART_SUB_FAT = 0x81, PART_SUB_SPIFFS = 0x82, PART_SUB_ANY = 0xFF };

/* Loads and checks the table (MD5 entry verified when present). Returns
 * number of entries, -1 on bad table. */
int                partition_load(void);
const partition_t *partition_get(int i);
const partition_t *partition_find(uint8_t type, uint8_t subtype, const char *label);   /* label may be NULL */

/* bounds-checked access relative to the partition start */
int partition_read(const partition_t *p, uint32_t off, void *buf, size_t len);
int partition_write(const partition_t *p, uint32_t off, const void *buf, size_t len);
int partition_erase(const partition_t *p, uint32_t off, size_t len);   /* 4 KB aligned */
