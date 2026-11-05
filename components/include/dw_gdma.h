/* DW-GDMA (ESP32-P4 only, TRM chapter 8): the Synopsys AXI DMA controller with 4 channels that feeds the MIPI DSI bridge
 * and drains the MIPI CSI bridge / ISP, and also does memory to memory. A transfer is a list of 64-byte-aligned link-list
 * items (one block each). Polling API plus per-block / full-transfer flags. Cache maintenance of the buffers is the
 * caller's job (cache_p4.h); the items are written back by dwg_start(). */
#pragma once
#include "ndf.h"

typedef struct dw_lli {
    uint32_t sar_lo, sar_hi, dar_lo, dar_hi, block_ts, rsv14, llp_lo, llp_hi, ctl_lo, ctl_hi, sstat, dstat, status_lo, status_hi, rsv38, rsv3c;
} __attribute__((aligned(64))) dw_lli_t;

typedef enum { DWG_MEM, DWG_DSI, DWG_CSI, DWG_ISP } dwg_role_t;          /* the peripherals' FIFOs are addressed at the bridge memory */
enum { DWG_WIDTH_8, DWG_WIDTH_16, DWG_WIDTH_32, DWG_WIDTH_64, DWG_WIDTH_128, DWG_WIDTH_256, DWG_WIDTH_512 };
enum { DWG_ITEMS_1, DWG_ITEMS_4, DWG_ITEMS_8, DWG_ITEMS_16, DWG_ITEMS_32, DWG_ITEMS_64, DWG_ITEMS_128, DWG_ITEMS_256, DWG_ITEMS_512, DWG_ITEMS_1024 };

typedef struct {
    uint32_t src, dst;            /* addresses */
    uint32_t items;               /* block size in transfers of `width` */
    uint8_t  width;               /* DWG_WIDTH_* (both sides) */
    uint8_t  src_items, dst_items;/* burst sizes: DWG_ITEMS_* */
    uint8_t  burst_len;           /* AXI burst length (0: default) */
    bool     src_fixed, dst_fixed;/* address does not increment (FIFO side) */
    bool     last, irq;           /* last item of the list; block-done interrupt flag */
} dwg_block_t;

int  dwg_init(void);
void dwg_lli_fill(dw_lli_t *l, const dwg_block_t *b, dw_lli_t *next);
/* channel 0..3: roles decide the flow (M2M / M2P / P2M, DMA as flow controller) and the hardware handshake */
int  dwg_open(int ch, dwg_role_t src, dwg_role_t dst, int priority);
int  dwg_start(int ch, dw_lli_t *first);                                    /* run a linked list */
int  dwg_wait(int ch, uint32_t timeout_us);                                /* full transfer done: 0 */
uint32_t dwg_status(int ch);                                               /* channel interrupt status bits */
void dwg_stop(int ch);                                                     /* abort the channel */
int  dwg_m2m(void *dst, const void *src, size_t n);                        /* convenience: one block, channel 3 */
