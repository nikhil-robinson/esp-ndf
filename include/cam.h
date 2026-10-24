/* Camera (DVP) input through LCD_CAM: 8/16-bit data, PCLK, VSYNC, DE (HREF); frames by DMA, one frame =
 * VSYNC to VSYNC. Optional XCLK output for sensors that need a master clock. */
#pragma once
#include "ndf.h"

typedef struct {
    int8_t data[16], pclk, vsync, de, xclk;   /* xclk < 0: none */
    uint8_t bits;                             /* 8 or 16 */
    uint32_t xclk_hz;
} cam_cfg_t;

int  cam_init(const cam_cfg_t *c);
/* Arm a capture into buf (internal RAM, <= 32 KB); cam_wait returns the bytes of the frame, -1 timeout. */
int  cam_capture(void *buf, size_t max);
int  cam_wait(uint32_t timeout_us);
