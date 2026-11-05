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
    ppa_pic_t pb = { b, H, W, 0, 0, H, W, PPA_RGB565 };      /* the rotated picture is H wide, W high */
    err += ppa_srm(&pa, &pb, 16, 16, 90, false, false) != 0;           /* rotate 90, 1:1: b is H wide, W high in the top-left */
    {   /* out is H wide, W high; try both senses of rotation */
        int cw = 0, ccw = 0;
        for (int y = 0; y < W; y++) for (int x = 0; x < H; x++) { cw += b[y * H + x] != a[(H - 1 - x) * W + y]; ccw += b[y * H + x] != a[x * W + (W - 1 - y)]; }
        printf("rot90 wrong pixels: clockwise-map %d, anticlockwise-map %d of %d\n", cw, ccw, W * H); err += cw && ccw; }
    printf("srm 90: out %d x %d\n", pb.w, pb.h);
    ppa_pic_t pc = { c, 2 * W, 2 * H, 0, 0, 2 * W, 2 * H, PPA_RGB565 };
    ppa_pic_t pi = { a, W, H, 0, 0, W, H, PPA_RGB565 };
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) a[y * W + x] = x < W / 2 ? 0x1234 : 0xABCD;       /* two flat halves */
    cache_dma_writeback(a, sizeof a);
    err += ppa_srm(&pi, &pc, 32, 32, 0, false, false) != 0;           /* 2x */
    err += pc.w != 2 * W || pc.h != 2 * H;
    {   /* scaling interpolates: the interior of the two constant halves of (a) must come out unchanged, doubled in size */
        int bad = 0, n = 0;
        for (int y = 2; y < 2 * H - 2; y++) for (int x = 0; x < 2 * W; x++) {
            int sx = x / 2; if (sx >= W / 2 - 2 && sx <= W / 2 + 1) continue;
            n++; bad += c[y * 2 * W + x] != a[(y / 2) * W + sx];
        }
        printf("scale 2x: %d of %d interior pixels wrong\n", bad, n); err += bad > 0; }
    for (int i = 0; i < W * H; i++) argb[i] = 0x80FF0000u;            /* half transparent red over the picture in b */
    ppa_pic_t pf = { argb, W, H, 0, 0, W, H, PPA_ARGB8888 }, po = { c, 2 * W, 2 * H, 0, 0, W, H, PPA_RGB565 };
    err += ppa_blend(&pa, &pf, &po, (ppa_a_t){ PPA_ALPHA_KEEP, 0 }, (ppa_a_t){ PPA_ALPHA_KEEP, 0 }) != 0;
    printf("ppa test errors %d\n", err);
    return err != 0;
}
