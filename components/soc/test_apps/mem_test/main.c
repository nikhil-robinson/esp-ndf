/* Memory map: RTC_DATA/BSS/NOINIT semantics over a software reset; heap regions (internal + PSRAM), capability
 * allocation, malloc placement, data integrity, speed (sibling: bench/idf_mem). */
#include "console.h"
#include "heap.h"
#include "psram.h"
#include "sys.h"
#include "system.h"
#include "xtensa.h"
#include <string.h>

RTC_DATA_ATTR int rtc_data = 5;
RTC_BSS_ATTR int rtc_bss;
RTC_NOINIT_ATTR uint32_t rtc_noinit, rtc_step;

static uint32_t cyc(void *(*f)(size_t), size_t n)
{
    uint32_t best = ~0u;
    for (int i = 0; i < 16; i++) { uint32_t c = ccount(); void *p = f(n); free(p); c = ccount() - c; if (c < best) best = c; }
    return best;
}

int main(void)
{
    delay_ms(200);
    int fail = 0;
    if (rtc_step != 0x52544331u) {                                    /* first boot of the test */
        printf("%s RTC_DATA initialised (%d), RTC_BSS zero (%d); at %p / %p / %p\n", rtc_data == 5 && !rtc_bss ? "PASS" : "FAIL",
               rtc_data, rtc_bss, (void *)&rtc_data, (void *)&rtc_bss, (void *)&rtc_noinit);
        rtc_data = 6; rtc_bss = 1; rtc_noinit = 0xC0FFEE; rtc_step = 0x52544331u;
        sys_restart();
    }
    rtc_step = 0;
    int ok = rtc_data == 5 && !rtc_bss && rtc_noinit == 0xC0FFEE;
    printf("%s after a software reset: RTC_DATA re-initialised (%d), RTC_BSS zeroed (%d), RTC_NOINIT kept (%lx)\n",
           ok ? "PASS" : "FAIL", rtc_data, rtc_bss, (unsigned long)rtc_noinit); fail |= !ok;

    size_t in0 = heap_free();
    uint32_t sz = psram_init(2);
    int a = heap_add_psram((void *)PSRAM_VADDR, sz);
    size_t ps0 = heap_caps_free_size(HEAP_SPIRAM);
    uint8_t *big = malloc(100000), *small = malloc(100), *dma = heap_caps_malloc(4096, HEAP_DMA), *ext = heap_caps_malloc(65536, HEAP_SPIRAM);
    ok = !a && sz && big && (uintptr_t)big >= PSRAM_VADDR && small && (uintptr_t)small < 0x3FD00000u && dma &&
         (uintptr_t)dma < 0x3FD00000u && ext && (uintptr_t)ext >= PSRAM_VADDR;
    printf("%s PSRAM %lu KB in the heap (%lu free); malloc(100000) %p (PSRAM), malloc(100) %p, DMA %p, SPIRAM %p\n", ok ? "PASS" : "FAIL",
           (unsigned long)(sz >> 10), (unsigned long)ps0, big, small, dma, ext); fail |= !ok;
    for (int i = 0; i < 100000; i++) big[i] = (uint8_t)(i * 13);
    for (int i = 0; i < 65536; i++) ext[i] = (uint8_t)(i * 7);
    int bad = 0; for (int i = 0; i < 100000; i++) bad += big[i] != (uint8_t)(i * 13);
    for (int i = 0; i < 65536; i++) bad += ext[i] != (uint8_t)(i * 7);
    free(big); free(small); free(dma); free(ext);
    ok = !bad && heap_free() == in0 && heap_caps_free_size(HEAP_SPIRAM) == ps0;
    printf("%s data in PSRAM blocks intact (%d bad), all freed: internal %lu/%lu, PSRAM %lu/%lu\n", ok ? "PASS" : "FAIL", bad,
           (unsigned long)heap_free(), (unsigned long)in0, (unsigned long)heap_caps_free_size(HEAP_SPIRAM), (unsigned long)ps0); fail |= !ok;
    printf("speed: malloc+free 64 B %lu cycles, 4 KB %lu, 100 KB (PSRAM) %lu\n", (unsigned long)cyc(malloc, 64),
           (unsigned long)cyc(malloc, 4096), (unsigned long)cyc(malloc, 100000));
    printf("mem_test %s done\n", fail ? "FAIL" : "PASS");
    for (;;) {}
}
