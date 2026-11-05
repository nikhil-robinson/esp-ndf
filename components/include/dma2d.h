/* 2D-DMA (ESP32-P4 only, TRM chapter 7 "VDMA"): 4 TX (read memory) and 3 RX (write memory) channels that move 1-D buffers or
 * 2-D blocks of a picture between memory and the JPEG codec, the PPA or each other (memory to memory). A transfer is a
 * linked list of 24-byte descriptors; the engine fetches them, reads / writes the pixels, optionally reorders macro
 * blocks. Polling API; the peripheral drivers (jpeg.h, ppa.h) own the channels they use. Descriptors are cache-maintained by the
 * start calls; the caller writes back its TX pixel buffers before and invalidates its RX buffers after (cache_p4.h). */
#pragma once
#include "ndf.h"

#if defined(NDF_P4_REV) && NDF_P4_REV < 3
#define DMA2D_TX_CHANNELS 3      /* v1.x silicon: hw_ver1 dma2d_reg.h has OUT channels 0..2 and IN channels 0..1 */
#define DMA2D_RX_CHANNELS 2
#else
#define DMA2D_TX_CHANNELS 4
#define DMA2D_RX_CHANNELS 3
#endif

typedef struct dma2d_desc {
    uint32_t w0;                      /* vb_size[13:0] hb_length[27:14] err_eof[28] dma2d_en[29] suc_eof[30] owner[31] */
    uint32_t w1;                      /* va_size[13:0] ha_length[27:14] pbyte[31:28] */
    uint32_t w2;                      /* y[13:0] x[27:14] mode[28] */
    void *buf;
    struct dma2d_desc *next;
} __attribute__((aligned(8))) dma2d_desc_t;

typedef enum { DMA2D_PERIPH_JPEG, DMA2D_PERIPH_PPA_SRM, DMA2D_PERIPH_PPA_BLEND, DMA2D_PERIPH_M2M } dma2d_periph_t;
typedef enum { DMA2D_MB_NONE, DMA2D_MB_8_8, DMA2D_MB_8_16, DMA2D_MB_16_16 } dma2d_mb_t;
enum { DMA2D_PBYTE_0B5, DMA2D_PBYTE_1B0, DMA2D_PBYTE_1B5, DMA2D_PBYTE_2B0, DMA2D_PBYTE_3B0, DMA2D_PBYTE_4B0 };

typedef struct {
    dma2d_periph_t periph;
    int periph_id;                    /* TX: JPEG 0, PPA SRM 1, PPA blend background 2 / foreground 3; RX: JPEG 0, SRM 1, blend 2; -1: M2M (any free id) */
    dma2d_mb_t mb;                    /* macro block size for MODE_MULTIPLE descriptors (JPEG uses 8x8 .. 16x16) */
    bool reorder;                     /* macro block reorder (channel 0 only) */
    bool dscr_port;                   /* PPA SRM: block size comes from the descriptor port */
    int dscr_port_h, dscr_port_v;
} dma2d_cfg_t;

int  dma2d_init(void);                                              /* clock, reset, FIFOs; idempotent */
void dma2d_desc_1d(dma2d_desc_t *d, void *buf, uint32_t size, uint32_t len, bool eof, dma2d_desc_t *next);
void dma2d_desc_2d(dma2d_desc_t *d, void *buf, int ha, int va, int hb, int vb, int x, int y, int pbyte, bool multiple, bool eof, dma2d_desc_t *next);
int  dma2d_tx_start(int ch, const dma2d_cfg_t *c, dma2d_desc_t *first);   /* cache write back of descriptors, connect, start */
int  dma2d_rx_start(int ch, const dma2d_cfg_t *c, dma2d_desc_t *first);   /* RX: invalidates buffers afterwards in dma2d_rx_wait */
int  dma2d_tx_wait(int ch, uint32_t timeout_us);                    /* EOF: 0, error / timeout: -1 */
int  dma2d_rx_wait(int ch, uint32_t timeout_us, uint32_t *received); /* success EOF (or DONE of the last descriptor): 0 */
void dma2d_stop(int tx_ch, int rx_ch);                              /* abort + reset (-1: skip) */
uint32_t dma2d_rx_status(int ch);                                   /* raw interrupt bits */
int  dma2d_m2m(void *dst, const void *src, size_t n);              /* memory to memory with channel TX3 / RX2 */
