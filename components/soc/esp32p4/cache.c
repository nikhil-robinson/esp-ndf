/* Flash cache / XIP interface of cache.h for the P4. No flash cache mapping yet: code and data run from L2 RAM, flash
 * is reached through SPI1 (drivers/flash/esp32p4/flash.c), so there is nothing to stall, map or invalidate.
 * ponytail: when XIP arrives (P4 MMU + L1 caches, ESP-IDF v6.1 mmu_ll.h / cache_ll.h), these become real. */
#include "cache.h"

extern char _text_end[];

int  cache_xip_init(void) { return 0; }
bool cache_xip_active(void) { return false; }
void cache_invalidate_code(void) {}
void cache_invalidate_code_range(uint32_t flash_addr, size_t len) { (void)flash_addr; (void)len; }
uint32_t cache_flash_addr(const void *code) { (void)code; return 0; }
uint32_t cache_flash_op_begin(void) { return 0; }
void cache_flash_op_end(uint32_t state, uint32_t flash_addr, size_t len) { (void)state; (void)flash_addr; (void)len; }
bool cache_is_code_addr(uint32_t a) { return a >= 0x4FF80000u && a < (uint32_t)_text_end; }
