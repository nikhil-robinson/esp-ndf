#include "t.h"
#include "esp_flash.h"
#define TEST_ADDR 0xF00000
static uint8_t w[4096], r[4096];
void t_flash(void)
{
    uint32_t id; esp_flash_read_id(NULL, &id);
    uint32_t t = ccount(); esp_flash_erase_region(NULL, TEST_ADDR, 4096); uint32_t te = (ccount() - t) / 240;
    for (int i = 0; i < 4096; i++) w[i] = i * 31 + (i >> 8);
    t = ccount(); esp_flash_write(NULL, w, TEST_ADDR, 4096); uint32_t tw = (ccount() - t) / 240;
    t = ccount(); esp_flash_read(NULL, r, TEST_ADDR, 4096); uint32_t tr = (ccount() - t) / 240;
    int bad = 0; for (int i = 0; i < 4096; i++) bad += r[i] != w[i];
    printf("    JEDEC id %06lx; 4 KB: erase %lu us, write %lu us, read %lu us, %d bytes wrong\n", id, te, tw, tr, bad);
    esp_flash_erase_region(NULL, TEST_ADDR, 4096);
}
