/* PPA throughput in internal RAM, one call each, best of 8: fill 256x128 RGB565, scale 128x64 -> 256x128 RGB565, rotate 90 of 128x64 RGB565,
 * blend 128x64 ARGB8888 over ARGB8888. */
#include "ppa.h"
#include "console.h"
#include "cpu.h"
#include "sys.h"
#include "cache_p4.h"
static uint16_t big[256 * 128] __attribute__((aligned(64))), small[128 * 64] __attribute__((aligned(64))), rot[64 * 128] __attribute__((aligned(64)));
static uint32_t fg[128 * 64] __attribute__((aligned(64))), bg[128 * 64] __attribute__((aligned(64))), mix[128 * 64] __attribute__((aligned(64)));
#define US(c) ((c) / (sys_cpu_hz() / 1000000))
int main(void)
{
    for (int i = 0; i < 128 * 64; i++) { small[i] = (uint16_t)(i * 31); fg[i] = 0x80000000u | i; bg[i] = 0xFF000000u | (i * 7); }
    cache_dma_writeback(small, sizeof small); cache_dma_writeback(fg, sizeof fg); cache_dma_writeback(bg, sizeof bg);
    ppa_pic_t pbig = { big, 256, 128, 0, 0, 256, 128, PPA_RGB565 }, ps = { small, 128, 64, 0, 0, 128, 64, PPA_RGB565 }, pr = { rot, 64, 128, 0, 0, 64, 128, PPA_RGB565 };
    ppa_pic_t pf = { fg, 128, 64, 0, 0, 128, 64, PPA_ARGB8888 }, pb = { bg, 128, 64, 0, 0, 128, 64, PPA_ARGB8888 }, pm = { mix, 128, 64, 0, 0, 128, 64, PPA_ARGB8888 };
    uint32_t tf = ~0u, ts = ~0u, tr = ~0u, tb = ~0u, t; int bad = 0;
    for (int k = 0; k < 8; k++) {
        t = ccount(); bad += ppa_fill(&pbig, 0x00FF00FF) != 0; t = ccount() - t; if (t < tf) tf = t;
        pbig.w = 256; pbig.h = 128;
        t = ccount(); bad += ppa_srm(&ps, &pbig, 32, 32, 0, false, false) != 0; t = ccount() - t; if (t < ts) ts = t;
        t = ccount(); bad += ppa_srm(&ps, &pr, 16, 16, 90, false, false) != 0; t = ccount() - t; if (t < tr) tr = t;
        t = ccount(); bad += ppa_blend(&pb, &pf, &pm, (ppa_a_t){ PPA_ALPHA_KEEP, 0 }, (ppa_a_t){ PPA_ALPHA_KEEP, 0 }) != 0; t = ccount() - t; if (t < tb) tb = t;
    }
    printf("\nppa_bench (us, best of 8): fill 256x128 RGB565 %lu | scale 2x 128x64 RGB565 %lu | rotate 90 128x64 RGB565 %lu | blend 128x64 ARGB8888 %lu | errors %d\n",
           US(tf), US(ts), US(tr), US(tb), bad);
    printf(bad ? "ppa_bench FAILED\n" : "ppa_bench done\n");
    for (;;) ;
}
