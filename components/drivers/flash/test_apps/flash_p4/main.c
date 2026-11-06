/* Flash speed on the P4 board: erase / program 4 KB and 64 KB, read 4 KB and 64 KB, all verified; same scratch area as flash_unit. */
#include "console.h"
#include "cpu.h"
#include "sys.h"
#include "flash.h"
#include <string.h>

#define AREA 0xF00000u
static uint8_t w[65536], r[65536];
static uint32_t us(uint32_t t) { return (ccount() - t) / (sys_cpu_hz() / 1000000); }

int main(void)
{
    int bad = 0;
    for (size_t i = 0; i < sizeof w; i++) w[i] = i * 31 + (i >> 8);
    printf("\nflash_p4: JEDEC %06lx, %lu MB, %d data lines, clock %lu MHz\n", flash_jedec_id(), flash_size() >> 20, flash_read_lines(), flash_clock_mhz());
    uint32_t t = ccount(); flash_erase(AREA, 4096); uint32_t e4 = us(t);
    t = ccount(); flash_erase(AREA, 65536); uint32_t e64 = us(t);
    t = ccount(); flash_write(AREA, w, 4096); uint32_t w4 = us(t);
    flash_erase(AREA, 65536);
    t = ccount(); flash_write(AREA, w, 65536); uint32_t w64 = us(t);
    uint32_t r4 = ~0u, r64 = ~0u;
    for (int k = 0; k < 4; k++) {
        memset(r, 0, sizeof r);
        t = ccount(); flash_read(AREA, r, 4096); uint32_t a = us(t); if (a < r4) r4 = a;
        t = ccount(); flash_read(AREA, r, 65536); uint32_t b = us(t); if (b < r64) r64 = b;
    }
    for (size_t i = 0; i < sizeof w; i++) bad += r[i] != w[i];
    printf("  erase 4 KB %lu us, 64 KB %lu us | write 4 KB %lu us, 64 KB %lu us | read 4 KB %lu us (%lu MB/s), 64 KB %lu us (%lu MB/s) | %d wrong\n",
           e4, e64, w4, w64, r4, 4096 / r4, r64, 65536 / r64, bad);
    flash_erase(AREA, 65536);
    if (!bad) printf("flash_p4 PASS\n");
    for (;;) ;
}
