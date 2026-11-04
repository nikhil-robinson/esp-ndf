/* Flash read speed: inspect the SPI1 clock, try faster settings, verify data. */
#include "flash.h"
#include "console.h"
#include "xtensa.h"
#include <string.h>
#include "crc32.h"

static uint8_t ref[4096] __attribute__((aligned(16))), buf[4096] __attribute__((aligned(16)));

static uint32_t t_read(void)
{
    uint32_t best = ~0u;
    for (int k = 0; k < 5; k++) { uint32_t t = ccount(); flash_read(0x1000, buf, 4096); t = ccount() - t; if (t < best) best = t; }
    return best;
}

int main(void)
{
    printf("\n=== flash read speed ===\n");
    printf("  SPI1 CLOCK %08lx  CORE_CLK_SEL %08lx | SPI0 CLOCK %08lx  CORE_CLK_SEL %08lx\n",
           REG(0x60002014), REG(0x600020EC), REG(0x60003014), REG(0x600030EC));
    flash_read(0x1000, ref, 4096);
    uint32_t t = t_read();
    /* verify against plain single-line reads, many sizes and alignments */
    int bad = 0;
    static uint8_t r2[700];
    for (int n = 1; n <= 600; n += 37)
        for (int o = 0; o < 4; o++) {
            uint32_t a = 0x1000 + n * 3 + o;
            memset(r2, 0xA5, sizeof r2);
            flash_read(a, r2 + o, n);
            if (memcmp(r2 + o, ref + (a - 0x1000), n) || r2[o + n] != 0xA5) bad++;
        }
    printf("  current: 4 KB in %lu cycles = %lu us, %lu MB/s, lines %d; size/offset checks: %d wrong\n", t, t / 240, 4096u * 240 / t, flash_read_lines(), bad);
    {   uint32_t t0 = ccount(), x = 0;
        for (int i = 0; i < 16; i++) x += REG(0x60002058 + 4 * i);
        uint32_t d = ccount() - t0;
        printf("  reading the 16 W registers: %lu cycles (%lu per read)%s\n", d, d / 16, x == 1 ? " " : "");
    }
    printf("  crc32 of flash 0x1000..0x1FFF: %08lx\n", crc32_le(0, ref, 4096));
    printf("=== done ===\n");
    for (;;) ;
}
