/* P4 data cache maintenance for DMA (soc/esp32p4/cache.c). The L1 D-cache sits in front of L2MEM and external memory:
 * write back before a DMA reads a buffer (peripheral TX, descriptors the engine fetches), write back + invalidate
 * around a DMA that writes it (peripheral RX, descriptors the engine updates). Buffers should start and end on 64-byte
 * lines: a line shared with other data is written back before it is dropped, never lost, but the neighbour's CPU write
 * during the transfer races the DMA. */
#pragma once
#include "ndf.h"
void cache_dma_writeback(const void *addr, size_t len);
void cache_dma_wbinv(const void *addr, size_t len);
int cache_flash_read(uint32_t flash_addr, void *dst, size_t len);   /* flash contents through the cache window; -1 before the cache runs */
