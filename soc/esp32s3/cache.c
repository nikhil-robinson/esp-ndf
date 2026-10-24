/* ESP32-S3 flash cache + MMU for code from flash (cache.h). TRM ch. 4 (cache, MMU) and 15.3.2.2 (SRAM0 to ICACHE).
 * Same steps as IDF's bootloader (bootloader_utility.c set_cache_and_start_app, hal cache_ll / mmu_ll), on our own
 * registers (where IDF calls ROM functions, those were read from the chip's ROM): the MMU is one table for both buses, entry i = 64 KB page i at 0x3C000000 + 64K*i (data) and at
 * 0x42000000 + 64K*i (instructions). flash.c maps the whole flash 1:1 (entry i = flash page i), so code linked at
 * 0x42000000 + offset runs straight from that flash offset. Everything here is in RAM. */
#include "cache.h"
#include "flash.h"
#include "soc.h"
#include "xtensa.h"

#define ICACHE_CTRL       0x600C4060u      /* b0 enable, b1 8-way, b2 32 KB, b3 32-byte lines */
#define ICACHE_CTRL1      0x600C4064u      /* b0 / b1: instruction bus of core 0 / 1 shut */
#define ICACHE_SYNC       0x600C4088u      /* b0 invalidate, b1 done */
#define ICACHE_SYNC_ADDR  0x600C408Cu
#define ICACHE_SYNC_SIZE  0x600C4090u
#define CACHE_CONNECT_1   0x600C1004u      /* [3:0]: cache data arrays on SRAM0 = block1 << 2 | block0 (ROM: 16 KB 0x1, 32 KB 0x9) */
#define SRAM_USAGE_1      0x600C1014u      /* [1:0]: SRAM0 block 0 / 1 to the CPU (clear: to the instruction cache) */
#define FLASH_IBUS        0x42000000u

extern char _text_end[], _flash_text_start[], _flash_text_end[];
static bool active;

static void icache_sync(uint32_t vaddr, uint32_t len)   /* len 0: everything */
{
    REG(ICACHE_SYNC_ADDR) = vaddr; REG(ICACHE_SYNC_SIZE) = len;
    REG(ICACHE_SYNC) = BIT(0);
    uint32_t t = ccount();
    while (!(REG(ICACHE_SYNC) & BIT(1)) && ccount() - t < 2400000) {}
}

int cache_xip_init(void)
{
    if (!flash_cache_map()) return -1;                   /* SPI0 + MMU + data cache (flash.c) */
    REG_CLR(ICACHE_CTRL, BIT(0));
    uint32_t big = NDF_ICACHE_KB == 32;                  /* as ROM rom_config_instruction_cache_mode (disassembled) */
    REG(CACHE_CONNECT_1) = (REG(CACHE_CONNECT_1) & ~0xFu) | (big ? 0x9 : 0x1);   /* data arrays on block 0 (+ block 1) */
    REG(SRAM_USAGE_1) = (REG(SRAM_USAGE_1) & ~3u) | (big ? 0 : BIT(1));         /* 32 KB: both blocks, 16 KB: block 0 */
    REG(ICACHE_CTRL) = (REG(ICACHE_CTRL) & ~0xEu) | BIT(1) | big << 2 | BIT(3);   /* 8-way, size, 32-byte lines */
    icache_sync(0, 0);
    REG_SET(ICACHE_CTRL, BIT(0));
    REG_CLR(ICACHE_CTRL1, BIT(0) | BIT(1));             /* both cores may fetch from flash */
    active = true;
    return 0;
}

bool cache_xip_active(void) { return active; }
void cache_invalidate_code(void) { if (active) icache_sync(0, 0); }
void cache_invalidate_code_range(uint32_t a, size_t len) { if (active && len) icache_sync(FLASH_IBUS + a, (uint32_t)len); }

uint32_t cache_flash_op_begin(void)
{
    uint32_t ps;
    __asm__ volatile("rsil %0, 3" : "=r"(ps) :: "memory");   /* levels 1-3 off: their handlers may live in flash */
    uint32_t st = ps;
    if (active && core_id() == 0 && (REG(SYSTEM_CORE1_CTRL0) & BIT(1)) && !(REG(SYSTEM_CORE1_CTRL0) & BIT(0))) {
        REG_SET(SYSTEM_CORE1_CTRL0, BIT(0));            /* stall core 1 while the flash is busy */
        st |= 1u << 31;                                 /* PS uses the low bits only */
    }
    return st;
}

void cache_flash_op_end(uint32_t st, uint32_t flash_addr, size_t len)
{
    if (active) icache_sync(FLASH_IBUS + flash_addr, (uint32_t)len ? (uint32_t)len : 1);
    if (st & (1u << 31)) REG_CLR(SYSTEM_CORE1_CTRL0, BIT(0));
    irq_unlock(st & ~(1u << 31));
}

bool cache_is_code_addr(uint32_t a)
{
    return (a >= 0x40378000u && a < (uint32_t)_text_end) ||
           (a >= (uint32_t)_flash_text_start && a < (uint32_t)_flash_text_end);
}
