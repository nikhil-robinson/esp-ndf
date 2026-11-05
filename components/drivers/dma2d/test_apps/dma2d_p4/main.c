#include "dma2d.h"
#include "console.h"
#include "cache_p4.h"
#include <string.h>

#define W 64
#define H 32
static uint8_t pic[W * H] __attribute__((aligned(64))), dst[W * H] __attribute__((aligned(64)));
static uint8_t a[1024] __attribute__((aligned(64))), b[1024] __attribute__((aligned(64)));
static dma2d_desc_t td, rd;

int main(void)
{
    int err = 0;
    for (int i = 0; i < 1024; i++) a[i] = (uint8_t)(i * 7 + 3);
    err += dma2d_m2m(b, a, sizeof a) != 0;
    err += memcmp(a, b, sizeof a) != 0;
    printf("1-D copy: %s\n", err ? "FAIL" : "ok");
    for (int i = 0; i < W * H; i++) pic[i] = (uint8_t)(i * 13);
    memset(dst, 0, sizeof dst);
    /* crop 24 x 8 at (x 8, y 4) of the W x H picture into a packed 24 x 8 buffer, 1 byte per pixel */
    dma2d_cfg_t c = { .periph = DMA2D_PERIPH_M2M, .periph_id = -1 };
    dma2d_desc_2d(&td, pic, W, H, 24, 8, 8, 4, DMA2D_PBYTE_1B0, false, true, 0);
    dma2d_desc_2d(&rd, dst, 24, 8, 24, 8, 0, 0, DMA2D_PBYTE_1B0, false, true, 0);
    err += dma2d_init() || dma2d_rx_start(2, &c, &rd) || dma2d_tx_start(3, &c, &td);
    err += dma2d_tx_wait(3, 1000000) || dma2d_rx_wait(2, 1000000, 0);
    cache_dma_wbinv(dst, sizeof dst);
    dma2d_stop(3, 2);
    for (int y = 0; y < 8; y++) err += memcmp(&dst[y * 24], &pic[(y + 4) * W + 8], 24) != 0;
    printf("dma2d test errors %d\n", err);
    return err != 0;
}
