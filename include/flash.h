/* SPI NOR flash through SPI1. Chips: ESP-IDF's list (generic/XMC, GD, ISSI, MXIC, Winbond, Boya, TH, FM), detected by
 * JEDEC ID + SFDP; MXIC octal (OPI STR) when eFuse FLASH_TYPE says 8 lines. Every wait has a timeout. Erase granularity 4 KB (larger erase types used by flash_erase).
 * With suspend (NDF_FLASH_SUSPEND), code and data in flash keep running on both cores during erase/program. */
#pragma once
#include "ndf.h"

#define FLASH_SECTOR 4096
#define FLASH_PAGE   256

typedef struct {
    uint32_t jedec_id, size;                 /* manufacturer << 16 | type << 8 | capacity; bytes */
    uint8_t  sfdp;                           /* basic SFDP table length in dwords, 0 = no SFDP */
    uint8_t  erase_cmd[4];                   /* erase types, smallest first (SFDP, else 4 KB 0x20 / 64 KB 0xD8) */
    uint32_t erase_size[4];                  /* 0 = unused */
    bool     suspend;                        /* hardware auto-suspend in use (IDF chip list, SFDP, NDF_FLASH_SUSPEND) */
    uint8_t  sus_cmd, res_cmd;
    uint8_t  qe_method;                      /* how the quad-enable bit is read/written (flash.c chip table) */
    bool     quad;                           /* QE set: quad reads */
    bool     addr4;                          /* larger than 16 MB: 4-byte address commands above 16 MB */
    bool     octal;                          /* MXIC octal flash in OPI STR (eFuse FLASH_TYPE) */
} flash_info_t;
const flash_info_t *flash_info(void);         /* read from the chip once (SFDP) */

uint32_t flash_jedec_id(void);                        /* manufacturer << 16 | type << 8 | capacity */
uint32_t flash_size(void);                            /* bytes, from the JEDEC capacity byte */
int      flash_read(uint32_t addr, void *buf, size_t len);
int      flash_read_raw(uint32_t addr, void *buf, size_t len);   /* plain SPI1 reads, no init (panic path, < 16 MB) */
int      flash_erase_sector(uint32_t addr);           /* addr rounded down to 4 KB */
int      flash_erase(uint32_t addr, size_t len);      /* 4 KB aligned; uses the largest erase type that fits */
int      flash_erase_chip(void);                       /* whole chip (pauses flash code: not suspendable) */
int      flash_unique_id(uint8_t id[8]);               /* 64-bit unique ID (0x4B) */
int      flash_write(uint32_t addr, const void *buf, size_t len);   /* area must be erased */
int      flash_read_lines(void);
uint32_t flash_clock_mhz(void);                      /* 80, or 120 when timing tuning found a window (NDF_FLASH_MHZ) */                       /* 2 = dual, 4 = quad (after first read) */
bool     flash_cached(void);                           /* reads go through the data cache (fast path) */
bool     flash_cache_map(void);                        /* map the whole flash through the MMU (used by cache.c) */
/* With code from flash (NDF_XIP), erase/write pause flash code: call them from core 0 (core 1 is stalled meanwhile). */
