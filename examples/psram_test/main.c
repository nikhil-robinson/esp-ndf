/* PSRAM test: init at 40 / 80 MHz, mode registers, full-size pattern write/read, bandwidth (CPU through the cache) */
#include "console.h"
#include "sys.h"
#include "xtensa.h"
#include "psram.h"
#include <string.h>
#ifndef LINE
#define LINE 64
#endif
static uint32_t sram_buf[8192];
int main(void)
{
    delay_ms(300);
    int div = 2; extern int psram_line_bytes; psram_line_bytes = LINE;
    uint32_t sz = psram_init(div);
    printf("dcache ctrl %08lx (line %d B)\n", (unsigned long)*(volatile uint32_t *)0x600C4000, LINE);
    uint8_t mr[2]; psram_cmd(0x4040, 0, 8, 0, 0, mr, 2);
    printf("psram init %lu bytes at 160/%d MHz, MR0 now %02x\n", (unsigned long)sz, div, mr[0]);
    if (!sz) { printf("=== done\n"); for (;;) {} }
    volatile uint32_t *p = (volatile uint32_t *)PSRAM_VADDR; uint32_t n = sz / 4;
    uint32_t t0 = ccount();
    for (uint32_t k = 0; k < n; k++) p[k] = k * 2654435761u;
    psram_writeback((void *)p, sz); uint32_t t1 = ccount();
    psram_invalidate((void *)p, sz);
    uint32_t bad = 0, t2 = ccount();
    for (uint32_t k = 0; k < n; k++) if (p[k] != k * 2654435761u) bad++;
    uint32_t t3 = ccount();
    printf("pattern %lu words, %lu bad | write %.1f MB/s read %.1f MB/s (CPU word loop)\n", (unsigned long)n, (unsigned long)bad,
           sz / ((t1 - t0) / 240.0), sz / ((t3 - t2) / 240.0));
    uint32_t t4 = ccount(); for (int r = 0; r < 32; r++) memcpy((void *)(PSRAM_VADDR + (uint32_t)r * sizeof sram_buf), sram_buf, sizeof sram_buf);
    psram_writeback((void *)PSRAM_VADDR, 32 * sizeof sram_buf); uint32_t t5 = ccount();
    psram_invalidate((void *)PSRAM_VADDR, 32 * sizeof sram_buf);
    uint32_t t6 = ccount(); for (int r = 0; r < 32; r++) memcpy(sram_buf, (void *)(PSRAM_VADDR + (uint32_t)r * sizeof sram_buf), sizeof sram_buf); uint32_t t7 = ccount();
    printf("memcpy SRAM->PSRAM %.1f MB/s, PSRAM->SRAM %.1f MB/s (1 MiB)\n", 32 * sizeof sram_buf / ((t5 - t4) / 240.0), 32 * sizeof sram_buf / ((t7 - t6) / 240.0));
    printf("=== done\n");
    for (;;) {}
}
