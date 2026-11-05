/* Image Signal Processor (ESP32-P4 only, TRM chapter 38): the pipeline between the MIPI CSI bridge and memory (RAW8/10/12 in;
 * RAW8, RGB888, RGB565, YUV422, YUV420 out): demosaic, colour correction matrix, colour (saturation / hue / contrast /
 * brightness), sharpen, YUV conversion. Capture with csi.h after isp_start(): the bridge then delivers the processed frames, so
 * csi_init() takes the OUTPUT bits per pixel (24 RGB888, 16 RGB565 / YUV422, 12 YUV420, 8 RAW8). Registers that are shadowed
 * take effect at the next frame (isp_update()). Statistics blocks (AE, AF, AWB, histogram), LSC, BF, DPC, gamma curves and
 * crop are separate calls below. */
#pragma once
#include "ndf.h"

typedef enum { ISP_IN_RAW8, ISP_IN_RAW10, ISP_IN_RAW12 } isp_in_t;
typedef enum { ISP_OUT_RAW8, ISP_OUT_RGB888, ISP_OUT_RGB565, ISP_OUT_YUV422, ISP_OUT_YUV420 } isp_out_t;
typedef enum { ISP_BAYER_BGGR, ISP_BAYER_GBRG, ISP_BAYER_GRBG, ISP_BAYER_RGGB } isp_bayer_t;
typedef struct {
    int h_res, v_res;
    isp_in_t in; isp_out_t out; isp_bayer_t bayer;
    int clk_khz;                  /* ISP clock (PLL_F240M / div), e.g. 80000 */
    bool yuv_bt709, yuv_full;     /* YUV conversion standard and range (default BT.601, limited) */
    bool csi_has_line_packets;    /* the sensor sends line start / end packets */
    bool byte_swap;
} isp_cfg_t;

int  isp_init(const isp_cfg_t *c);               /* clocks, reset, format, size, input from the CSI bridge */
void isp_start(void);                            /* pipeline on */
void isp_stop(void);
void isp_update(void);                           /* latch the shadowed settings at the next frame */
void isp_ccm(const int16_t m[3][3]);             /* colour correction matrix, 13-bit signed register words (hardware fixed point) */
void isp_ccm_enable(bool on);
void isp_color(uint8_t saturation, uint8_t hue, uint8_t contrast, uint8_t brightness);   /* raw register fields; enables the block */
void isp_demosaic_grad_ratio(uint8_t r);         /* 0..63, edge direction threshold */
void isp_sharpen_enable(bool on);
void isp_blc_enable(bool on);
void isp_wbg_enable(bool on);
void isp_crop_enable(bool on);
uint32_t isp_errors(void);                       /* data-type / FIFO / size error flags (INT_RAW low bits) */
