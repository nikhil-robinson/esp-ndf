#include "sys.h"
#include "test.h"
#include "flash.h"

#define TEST_ADDR 0xF00000      /* scratch sector, far above bootloader/partitions/app */
static uint8_t w[FLASH_SECTOR], r[FLASH_SECTOR];

void test_flash(void)
{
    uint32_t id = flash_jedec_id();
    printf("    JEDEC id %06lx, size %lu MB\n", id, flash_size() >> 20);
    CHECK(flash_size() == 16u << 20);
    uint32_t t = ccount();
    CHECK(flash_erase_sector(TEST_ADDR) == 0);
    uint32_t te = (ccount() - t) / (sys_cpu_hz() / 1000000);
    flash_read(TEST_ADDR, r, FLASH_SECTOR);
    int ff = 0; for (int i = 0; i < FLASH_SECTOR; i++) ff += r[i] == 0xFF;
    CHECK(ff == FLASH_SECTOR);
    for (int i = 0; i < FLASH_SECTOR; i++) w[i] = i * 31 + (i >> 8);
    t = ccount(); CHECK(flash_write(TEST_ADDR, w, FLASH_SECTOR) == 0); uint32_t tw = (ccount() - t) / (sys_cpu_hz() / 1000000);
    t = ccount(); flash_read(TEST_ADDR, r, FLASH_SECTOR); uint32_t tr = (ccount() - t) / (sys_cpu_hz() / 1000000);
    int bad = 0; for (int i = 0; i < FLASH_SECTOR; i++) bad += r[i] != w[i];
    printf("    read mode: %d data lines\n", flash_read_lines());
    printf("    4 KB: erase %lu us, write %lu us, read %lu us, %d bytes wrong (erased %d/4096 = 0xFF)\n", te, tw, tr, bad, ff);
    CHECK(bad == 0);
    flash_erase_sector(TEST_ADDR);
}

NDF_UNIT(010, flash);
