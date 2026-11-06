/* Flash cache + MMU for code and read-only data run from flash (cache.h), P4 hw_ver3. ESP-IDF v6.1 mmu_ll.h (flash MMU
 * = SPI0 index / content registers, 64 KB pages, valid bit 12), cache_hal.c / cache_ll.h (the cache switches are ROM
 * functions at fixed addresses, esp32p4.rom.ld) and bootloader_utility.c (L2 cache on for external memory). One MMU
 * table maps the unified 0x40000000-0x44000000 window; the code window (link.ld.in XIP_VWIN, 1 MB at 0x43F00000, entries
 * 1008-1023) is mapped onto the image's flash copy at NDF_XIP_ADDR. Flash reads go through the L1 I/D caches and the L2
 * cache; the SPI0 read mode is the one the ROM left (assumption, docs/P4.md). Everything here runs from RAM. */
#include "cache.h"
#include "cpu.h"
#include "sys.h"
#include "soc.h"

extern char _text_end[], _flash_text_start[], _flash_text_end[];

#define FL_MMU_CONTENT  (HPPERIPH0 + 0x8C000 + 0x37C)
#define FL_MMU_INDEX    (HPPERIPH0 + 0x8C000 + 0x380)
#define FL_MMU_VALID    BIT(12)
#define XIP_BASE        0x40000000u
#define VWIN            0x3F00000u
#define MAP_ICACHE      (BIT(0) | BIT(1))
#define MAP_DCACHE      BIT(4)
#define MAP_L2          BIT(5)
#define ROM_INVALIDATE_ALL(m)         ((int (*)(uint32_t))0x4fc00404)(m)
#define ROM_INVALIDATE_ADDR(m, a, n)  ((int (*)(uint32_t, uint32_t, uint32_t))0x4fc003e4)((m), (a), (n))
#define ROM_ENABLE_ICACHE0(al)        ((void (*)(uint32_t))0x4fc004d4)(al)
#define ROM_ENABLE_ICACHE1(al)        ((void (*)(uint32_t))0x4fc004e4)(al)
#define ROM_ENABLE_DCACHE(al)         ((void (*)(uint32_t))0x4fc004f4)(al)
#define ROM_ENABLE_L2(al)             ((void (*)(uint32_t))0x4fc00504)(al)

static bool active;
static uint32_t xip_phys = NDF_XIP_ADDR;

int cache_xip_init(void)
{
    if (REG(SOC_OTA_NOTE) == 0x5341544Fu) xip_phys = REG(SOC_OTA_NOTE + 8);   /* booted from an OTA slot (loader note) */
    for (uint32_t i = 0; i < (NDF_XIP_SIZE >> 16); i++) {
        REG(FL_MMU_INDEX) = (VWIN >> 16) + i;
        REG(FL_MMU_CONTENT) = ((xip_phys >> 16) + i) | FL_MMU_VALID;
    }
#if NDF_P4_REV < 3     /* v1.x: the ROM left the L1 caches on and an Invalidate_All on the L1 maps faults; IDF (cache_hal_init) only enables L2 */
    ROM_INVALIDATE_ALL(MAP_L2);
    ROM_ENABLE_L2(0);
#else
    ROM_INVALIDATE_ALL(MAP_ICACHE | MAP_DCACHE | MAP_L2);
    ROM_ENABLE_L2(0); ROM_ENABLE_DCACHE(0); ROM_ENABLE_ICACHE0(0); ROM_ENABLE_ICACHE1(0);   /* cache_hal_init order */
#endif
    active = true;
    return 0;
}

bool cache_xip_active(void) { return active; }
void cache_invalidate_code(void) { if (active) ROM_INVALIDATE_ALL(MAP_ICACHE); }
static bool code_vaddr(uint32_t flash_addr, uint32_t *va)     /* flash offset -> code window address */
{
    if (flash_addr < xip_phys || flash_addr >= xip_phys + NDF_XIP_SIZE) return false;
    *va = XIP_BASE + VWIN + (flash_addr - xip_phys);
    return true;
}
void cache_invalidate_code_range(uint32_t a, size_t len)
{
    uint32_t va;
    if (active && len && code_vaddr(a, &va)) ROM_INVALIDATE_ADDR(MAP_ICACHE, va, (uint32_t)len);
}
uint32_t cache_flash_addr(const void *code) { return (uint32_t)code - XIP_BASE - VWIN + xip_phys; }

/* around SPI1 erase / program: no instruction fetch from flash meanwhile. Interrupts off on this core (every handler may
 * live in flash), the other core stalled while it runs (PMU stall code, as cpu1_start). */
uint32_t cache_flash_op_begin(void)
{
    uint32_t st = irq_lock();
    if (active && core_id() == 0 && sys_cpu1_running() && !(REG(HP_SYS_CORESTALLED_ST) & BIT(1))) {
        REG_FIELD(PMU_CPU_SW_STALL, 16, 8, 0x86);
        for (uint32_t t = ccount(); !(REG(HP_SYS_CORESTALLED_ST) & BIT(1)) && ccount() - t < 400000u; ) {}
        st |= 1u << 31;                                 /* irq_lock returns a threshold below 8: bit 31 flags the stall */
    }
    return st;
}

void cache_flash_op_end(uint32_t st, uint32_t flash_addr, size_t len)
{
    uint32_t va;
    if (active && code_vaddr(flash_addr, &va)) ROM_INVALIDATE_ADDR(MAP_ICACHE | MAP_DCACHE | MAP_L2, va, len ? (uint32_t)len : 1);
    if (st & (1u << 31)) REG_FIELD(PMU_CPU_SW_STALL, 16, 8, 0xFF);
    irq_unlock(st & ~(1u << 31));
}

/* Flash reads for flash.h through the cache: a 64 KB MMU window (entry RWIN below the code window) is pointed at the page being read,
 * its lines dropped from the L1 D-cache and L2 (the entry was another page a moment ago, or the flash was programmed), then a memcpy
 * from the window: the cache fills 64-byte lines over the flash controller that code fetches use, instead of one 64-byte SPI1
 * command per 25 us. 4 KB steps: interrupts are off for one step, the window is owned by one core at a time. */
#define RWIN    992u
static volatile uint32_t rwin_lk;
int cache_flash_read(uint32_t flash_addr, void *dst, size_t len)
{
    static bool l2_ready;
    if (!active && !l2_ready) {                                  /* RAM-only images: the XIP init did not run, bring the L2 up as it does */
        uint32_t ps = irq_lock();
#if NDF_P4_REV < 3
        ROM_INVALIDATE_ALL(MAP_L2); ROM_ENABLE_L2(0);
#else
        ROM_INVALIDATE_ALL(MAP_ICACHE | MAP_DCACHE | MAP_L2); ROM_ENABLE_L2(0); ROM_ENABLE_DCACHE(0);
#endif
        l2_ready = true;
        irq_unlock(ps);
    }
    uint8_t *d = dst;
    while (len) {
        uint32_t off = flash_addr & 0xFFFFu;
        size_t n = 0x10000u - off; if (n > 4096) n = 4096; if (n > len) n = len;
        uint32_t ps = irq_lock();
        while (__atomic_exchange_n(&rwin_lk, 1, __ATOMIC_ACQUIRE)) {}
        uint32_t va = XIP_BASE + (RWIN << 16);
        REG(FL_MMU_INDEX) = RWIN; REG(FL_MMU_CONTENT) = (flash_addr >> 16) | FL_MMU_VALID;
        ROM_INVALIDATE_ADDR(MAP_DCACHE | MAP_L2, va + off, (uint32_t)n);
        const uint32_t *sp = (const uint32_t *)(va + off);
        if (!((uintptr_t)d & 3) && !(off & 3) && !(n & 3)) for (size_t i = 0; i < n / 4; i++) ((uint32_t *)d)[i] = sp[i];
        else for (size_t i = 0; i < n; i++) d[i] = ((const volatile uint8_t *)sp)[i];
        __atomic_store_n(&rwin_lk, 0, __ATOMIC_RELEASE);
        irq_unlock(ps);
        d += n; flash_addr += n; len -= n;
    }
    return 0;
}

bool cache_is_code_addr(uint32_t a)
{
    return (a >= SOC_IMG_LO && a < (uint32_t)_text_end) ||
           (a >= (uint32_t)_flash_text_start && a < (uint32_t)_flash_text_end);
}

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
#if NDF_P4_REV < 3     /* v1.x: the SYNC registers do not write back as the v3 ones do (DMA read stale descriptors); the ROM calls do (measured) */
#define ROM_WB(a, s)     ((int (*)(uint32_t, uint32_t, uint32_t))0x4fc003f4)(BIT(4) | BIT(5), (uint32_t)(a), (uint32_t)(s))   /* Cache_WriteBack_Addr */
#define ROM_WBINV(a, s)  ((int (*)(uint32_t, uint32_t, uint32_t))0x4fc003fc)(BIT(4) | BIT(5), (uint32_t)(a), (uint32_t)(s))   /* Cache_WriteBack_Invalidate_Addr */
void cache_dma_writeback(const void *addr, size_t len) { if (len) { uint32_t ps = irq_lock(); ROM_WB(addr, len); irq_unlock(ps); } }
void cache_dma_wbinv(const void *addr, size_t len)     { if (len) { uint32_t ps = irq_lock(); ROM_WBINV(addr, len); irq_unlock(ps); } }
#else
void cache_dma_writeback(const void *addr, size_t len) { sync(BIT(2), addr, len); }
void cache_dma_wbinv(const void *addr, size_t len)     { sync(BIT(3), addr, len); }
#endif
