/* SPI NOR flash through SPI1 (the image runs from RAM, so the bus is free).
 * Standard commands; every wait has a timeout. Erase granularity 4 KB. */
#pragma once
#include "ndf.h"

#define FLASH_SECTOR 4096
#define FLASH_PAGE   256

uint32_t flash_jedec_id(void);                        /* manufacturer << 16 | type << 8 | capacity */
uint32_t flash_size(void);                            /* bytes, from the JEDEC capacity byte */
int      flash_read(uint32_t addr, void *buf, size_t len);
int      flash_erase_sector(uint32_t addr);           /* addr rounded down to 4 KB */
int      flash_write(uint32_t addr, const void *buf, size_t len);   /* area must be erased */
int      flash_read_lines(void);                       /* 2 = dual, 4 = quad (after first read) */
bool     flash_cached(void);                           /* reads go through the data cache (fast path) */
bool     flash_cache_map(void);                        /* map the whole flash through the MMU (used by cache.c) */
/* With code from flash (NDF_XIP), erase/write pause flash code: call them from core 0 (core 1 is stalled meanwhile). */
