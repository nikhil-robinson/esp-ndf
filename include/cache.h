/* Flash cache and MMU: code and read-only data executed/read from flash (XIP, build option XIP in app.mk). */
#pragma once
#include "ndf.h"

/* Map flash and turn the instruction cache on (called before main when NDF_XIP != 0). 0 on success. */
int  cache_xip_init(void);
bool cache_xip_active(void);
void cache_invalidate_code(void);              /* drop every instruction-cache line (benchmarks, after code updates) */
void cache_invalidate_code_range(uint32_t flash_addr, size_t len);   /* instruction-cache lines of a flash range */

/* Around SPI1 erase/program while code may run from flash: no code may be fetched from flash meanwhile.
 * begin masks interrupt levels 1-3 on this core (levels 4/5 stay live: their handlers must be in RAM) and stalls
 * the other core if it runs; end undoes that and invalidates the instruction and data cache over the flash range. */
uint32_t cache_flash_op_begin(void);
void     cache_flash_op_end(uint32_t state, uint32_t flash_addr, size_t len);

/* true if a is in this image's code (RAM or flash): used by backtraces */
bool cache_is_code_addr(uint32_t a);
