/* Flash cache / XIP interface of cache.h for the P4. No flash cache mapping yet: code and data run from L2 RAM, flash
 * is reached through SPI1 (drivers/flash/esp32p4/flash.c), so there is nothing to stall, map or invalidate.
 * ponytail: when XIP arrives (P4 MMU + L1 caches, ESP-IDF v6.1 mmu_ll.h / cache_ll.h), these become real. */
#include "cache.h"
#include "cpu.h"
#include "soc.h"

extern char _text_end[];

int  cache_xip_init(void) { return 0; }
bool cache_xip_active(void) { return false; }
void cache_invalidate_code(void) {}
void cache_invalidate_code_range(uint32_t flash_addr, size_t len) { (void)flash_addr; (void)len; }
uint32_t cache_flash_addr(const void *code) { (void)code; return 0; }
uint32_t cache_flash_op_begin(void) { return 0; }
void cache_flash_op_end(uint32_t state, uint32_t flash_addr, size_t len) { (void)state; (void)flash_addr; (void)len; }
bool cache_is_code_addr(uint32_t a) { return a >= 0x4FF80000u && a < (uint32_t)_text_end; }

/* ---- data cache maintenance for DMA (ESP-IDF v6.1 hw_ver3 cache_reg.h SYNC_*): L2MEM and external memory are cached by
 * the L1 data cache (the ROM makes them cacheable), a DMA engine sees memory, not the cache. Operations cover the lines
 * of [addr, addr + len) in the L1 D-cache and the L2 cache. Writeback-invalidate is the safe form for both directions
 * (a partly shared line is written back before it is dropped). ---- */
#include "cache_p4.h"

#define SYNC_CTRL   (CACHE_CTRL_BASE + 0x98)   /* b0 invalidate, b1 clean, b2 writeback, b3 writeback+invalidate (self-clearing), b4 done */
#define SYNC_MAP    (CACHE_CTRL_BASE + 0x9C)   /* b4 L1 D-cache, b5 L2 */
#define SYNC_ADDR   (CACHE_CTRL_BASE + 0xA0)
#define SYNC_SIZE   (CACHE_CTRL_BASE + 0xA4)

static void sync(uint32_t op, const void *addr, size_t len)
{
    if (!len) return;
    uint32_t ps = irq_lock();
    REG(SYNC_MAP) = BIT(4) | BIT(5);
    REG(SYNC_ADDR) = (uint32_t)(uintptr_t)addr;
    REG(SYNC_SIZE) = (uint32_t)len;
    REG(SYNC_CTRL) = op;
    for (uint32_t t = ccount(); !(REG(SYNC_CTRL) & BIT(4)) && ccount() - t < 40000000u; ) {}   /* bounded */
    irq_unlock(ps);
}
void cache_dma_writeback(const void *addr, size_t len) { sync(BIT(2), addr, len); }
void cache_dma_wbinv(const void *addr, size_t len)     { sync(BIT(3), addr, len); }
