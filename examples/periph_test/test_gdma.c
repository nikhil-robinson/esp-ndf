#include "test.h"
#include "gdma.h"

#define LEN 32768
extern uint8_t back[32768];                     /* test_fat.c: shared scratch, as src */
#define src ((uint32_t *)back)
static uint32_t dst[LEN / 4];

void test_gdma(void)
{
    for (int i = 0; i < LEN / 4; i++) { src[i] = i * 2654435761u; dst[i] = 0; }
    int ch = gdma_alloc();
    CHECK(ch >= 0);
    gdma_connect(ch, GDMA_M2M);
    uint32_t t = ccount();
    int rc = gdma_memcpy(ch, dst, src, LEN);
    CHECK(rc == 0);
    uint32_t dma = ccount() - t;
    int bad = 0;
    for (int i = 0; i < LEN / 4; i++) bad += dst[i] != src[i];
    METRIC("DMA copy 32 KB", "%lu cycles (%lu MB/s), %d words wrong", dma, (uint32_t)((uint64_t)LEN * 240 / dma), bad);
    CHECK(bad == 0);
    t = ccount();
    for (int i = 0; i < LEN / 4; i++) ((volatile uint32_t *)dst)[i] = src[i];
    uint32_t cpu = ccount() - t;
    METRIC("CPU word copy 32 KB", "%lu cycles (%lu MB/s)", cpu, (uint32_t)((uint64_t)LEN * 240 / cpu));
    gdma_free(ch);
}
