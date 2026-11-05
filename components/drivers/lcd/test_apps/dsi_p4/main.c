#include "dsi.h"
#include "psram.h"
#include "console.h"

#define W 800
#define H 480
int main(void)
{
    int err = 0;
    if (!psram_init(4)) { printf("dsi test: no psram, errors 1\n"); return 1; }
    uint16_t *fb = (uint16_t *)PSRAM_VADDR;
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) fb[y * W + x] = (uint16_t)(x * 31 / W << 11 | (y * 63 / H) << 5 | 0x0F);
    dsi_cfg_t c = { .lanes = 2, .lane_mbps = 500 };
    err += dsi_init(&c) != 0;
    dsi_video_t v = { .h_size = W, .v_size = H, .hsync = 20, .hbp = 50, .hfp = 50, .vsync = 4, .vbp = 12, .vfp = 12, .dpi_khz = 30000, .bpp = 16 };
    err += dsi_video_start(&v, fb) != 0;
    printf("dsi test errors %d\n", err);
    return err != 0;
}
