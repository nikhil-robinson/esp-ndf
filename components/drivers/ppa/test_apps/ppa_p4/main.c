#include "ppa.h"
#include "console.h"
#include "cache_p4.h"
#include <string.h>

#define W 64
#define H 48
static uint16_t a[W * H] __attribute__((aligned(64))), b[W * H] __attribute__((aligned(64))), c[2 * W * 2 * H] __attribute__((aligned(64)));
static uint32_t argb[W * H] __attribute__((aligned(64)));

int main(void)
{
    int err = 0;
    ppa_pic_t pa = { a, W, H, 0, 0, W, H, PPA_RGB565 };
    err += ppa_fill(&pa, 0x00FF00FF) != 0;                               /* magenta, given as RGB888 (the fixed pixel is converted to the output colour mode) */
    for (int i = 0; i < W * H; i++) err += a[i] != 0xF81F;
    printf("fill: %s\n", err ? "FAIL" : "ok");
    for (int i = 0; i < W * H; i++) a[i] = (uint16_t)(i * 31);
    cache_dma_writeback(a, sizeof a);
    ppa_pic_t pb = { b, W, H, 0, 0, W, H, PPA_RGB565 };
    err += ppa_srm(&pa, &pb, 16, 16, 90, false, false) != 0;           /* rotate 90, 1:1: b is H wide, W high in the top-left */
    printf("srm 90: out %d x %d\n", pb.w, pb.h);
    ppa_pic_t pc = { c, 2 * W, 2 * H, 0, 0, 2 * W, 2 * H, PPA_RGB565 };
    ppa_pic_t pi = { a, W, H, 0, 0, W, H, PPA_RGB565 };
    err += ppa_srm(&pi, &pc, 32, 32, 0, false, false) != 0;           /* 2x */
    err += pc.w != 2 * W || pc.h != 2 * H;
    for (int i = 0; i < W * H; i++) argb[i] = 0x80FF0000u;            /* half transparent red over the picture in b */
    ppa_pic_t pf = { argb, W, H, 0, 0, W, H, PPA_ARGB8888 }, po = { c, 2 * W, 2 * H, 0, 0, W, H, PPA_RGB565 };
    err += ppa_blend(&pa, &pf, &po, (ppa_a_t){ PPA_ALPHA_KEEP, 0 }, (ppa_a_t){ PPA_ALPHA_KEEP, 0 }) != 0;
    printf("ppa test errors %d\n", err);
    return err != 0;
}
