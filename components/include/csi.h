/* MIPI CSI-2 receiver (ESP32-P4 only, TRM chapter 42): D-PHY + CSI host + bridge, frames captured to memory by the DW-GDMA
 * (dw_gdma.h) with the bridge as flow controller. The pixel stream passes through unchanged (RAW8 / RAW10 / RAW12 packed as
 * sent, or RGB / YUV as the sensor sends); colour conversion and the ISP pipeline are isp.h. 1 or 2 lanes, 80..1500 Mbit/s.
 * The PHY needs the LDO channel 3 at 2.5 V (NDF_DSI_LDO_CHAN / _MV, shared with the DSI; 0: the board powers it). */
#pragma once
#include "ndf.h"

typedef struct {
    int lanes;                    /* 1 or 2 */
    int lane_mbps;                /* CSI lane bit rate of the sensor */
    int h_res, v_res;             /* active pixels / lines */
    int bits_per_pixel;           /* in the stream: 8 (RAW8), 10, 12, 16 (YUV422 / RGB565), 24 (RGB888) */
    int dt_min, dt_max;           /* accepted CSI data types: 0x12..0x2f for everything (0, 0: that range) */
    bool byte_swap;
} csi_cfg_t;

int  csi_init(const csi_cfg_t *c);
/* capture one frame into `buf` (h_res * v_res * bits_per_pixel / 8 bytes, 64-byte aligned); blocks until it is complete */
int  csi_capture(void *buf, uint32_t timeout_us);
void csi_stop(void);
uint32_t csi_errors(void);                       /* bridge overrun / discard / size error flags (INT_RAW) */
