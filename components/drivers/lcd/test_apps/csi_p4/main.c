#include "csi.h"
#include "isp.h"
#include "psram.h"
#include "console.h"

int main(void)
{
    int err = 0;
    if (!psram_init(4)) { printf("csi test: no psram, errors 1\n"); return 1; }
    uint8_t *fb = (uint8_t *)PSRAM_VADDR;
    csi_cfg_t c = { .lanes = 2, .lane_mbps = 400, .h_res = 640, .v_res = 480, .bits_per_pixel = 8 };
    isp_cfg_t ic = { .h_res = 640, .v_res = 480, .in = ISP_IN_RAW8, .out = ISP_OUT_RGB565, .bayer = ISP_BAYER_RGGB, .clk_khz = 80000 };
    err += isp_init(&ic) != 0;
    isp_start();
    c.bits_per_pixel = 16;                                  /* the bridge now delivers the ISP's RGB565 */
    err += csi_init(&c) != 0;
    err += csi_capture(fb, 1000000) != 0;
    printf("csi test errors %d (bridge flags 0x%lx)\n", err, (unsigned long)csi_errors());
    return err != 0;
}
