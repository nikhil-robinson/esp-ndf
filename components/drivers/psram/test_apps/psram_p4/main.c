#include "psram.h"
#include "gdma.h"
#include "console.h"
#include "sys.h"
#include "cpu.h"
#include <string.h>

static uint32_t src[1024] __attribute__((aligned(64)));

int main(void)
{
    uint32_t sz = psram_init(4);
    printf("psram %lu KB at 0x%08lx\n", (unsigned long)(sz >> 10), (unsigned long)PSRAM_VADDR);
    if (!sz) { printf("psram test: absent, errors 1\n"); return 1; }
    volatile uint32_t *p = (volatile uint32_t *)PSRAM_VADDR;
    uint32_t n = sz / 4, err = 0;
    for (uint32_t i = 0; i < n; i += 61) p[i] = i * 2654435761u;
    psram_writeback((void *)p, sz);
    psram_invalidate((void *)p, sz);
    for (uint32_t i = 0; i < n; i += 61) err += p[i] != i * 2654435761u;
    for (int i = 0; i < 1024; i++) src[i] = 0xA5000000u + i;
    uint32_t t = ccount();
    for (int k = 0; k < 64; k++) memcpy((void *)(PSRAM_VADDR + 0x1000), src, sizeof src);
    psram_writeback((void *)(PSRAM_VADDR + 0x1000), sizeof src);
    t = ccount() - t;
    printf("write %lu KB/s\n", (unsigned long)(64u * sizeof src / 1024u * (uint32_t)(sys_cpu_hz() / 1000) / (t ? t : 1) * 1000 / 1000));
    err += memcmp((void *)(PSRAM_VADDR + 0x1000), src, sizeof src) != 0;
    printf("psram test: %lu KB, errors %lu\n", (unsigned long)(sz >> 10), (unsigned long)err);
    return err != 0;
}
