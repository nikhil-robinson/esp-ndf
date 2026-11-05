/* MIPI DSI host (ESP32-P4 only, TRM chapter 43): D-PHY + DSI host + bridge, DCS / generic commands in command mode, and a
 * video-mode (DPI) panel refreshed from a frame buffer in PSRAM or internal RAM by the DW-GDMA (dw_gdma.h). One bus, 1 or 2
 * data lanes, up to 1.5 Gbit/s per lane. The PHY needs the on-chip LDO channel 3 at 2.5 V (NDF_DSI_LDO_CHAN / _MV, 0: the
 * board powers it). Panels (init sequences, timings) are the application's. */
#pragma once
#include "ndf.h"

typedef struct {
    int lanes;                    /* 1 or 2 */
    int lane_mbps;                /* bit rate per lane, 80 .. 1500 */
    bool clock_lane_hs;           /* keep the clock lane in HS (no auto LP between lines) */
} dsi_cfg_t;
typedef struct {
    int h_size, v_size;           /* active pixels */
    int hsync, hbp, hfp;          /* horizontal pulse, back porch, front porch (pixels) */
    int vsync, vbp, vfp;          /* in lines */
    int dpi_khz;                  /* pixel clock */
    int bpp;                      /* 16 (RGB565) or 24 (RGB888) in the frame buffer and on the link */
    int vc;                       /* virtual channel 0..3 */
} dsi_video_t;

int  dsi_init(const dsi_cfg_t *c);
int  dsi_dcs_write(int vc, uint8_t cmd, const void *param, size_t n);      /* DCS short / long write in LP mode */
int  dsi_dcs_read(int vc, uint8_t cmd, void *buf, size_t n);
/* program the video timing, start the refresh of `fb` (h_size * v_size * bpp / 8 bytes, 64-byte aligned) */
int  dsi_video_start(const dsi_video_t *v, void *fb);
void dsi_video_stop(void);
uint32_t dsi_underruns(void);                                              /* FIFO underrun events since start */
