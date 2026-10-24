/* General DMA: 5 channels, each an IN (peripheral -> memory) and OUT
 * (memory -> peripheral) half, linked-list descriptors. Buffers and
 * descriptors must be in internal DRAM (0x3FC8_0000..0x3FCF_FFFF). */
#pragma once
#include "ndf.h"

typedef struct gdma_desc {
    uint32_t size : 12, length : 12, : 4, err_eof : 1, : 1, suc_eof : 1, owner : 1;
    void *buf;
    struct gdma_desc *next;
} gdma_desc_t;

#define GDMA_DESC_MAX 4092                 /* bytes per descriptor (word aligned) */

enum { GDMA_SPI2 = 0, GDMA_SPI3 = 1, GDMA_I2S0 = 3, GDMA_I2S1 = 4, GDMA_LCD_CAM = 5,
       GDMA_AES = 6, GDMA_SHA = 7, GDMA_ADC = 8, GDMA_RMT = 9, GDMA_M2M = 63 };

int  gdma_alloc(void);                     /* returns channel 0..4, -1 if none */
void gdma_free(int ch);
void gdma_connect(int ch, int periph);     /* both halves; GDMA_M2M = memory to memory */

/* Chain buf into descs (as many as needed); last one gets EOF if eof.
 * Returns descriptors used, or -1 if n is too small. */
int  gdma_chain(gdma_desc_t *d, int n, void *buf, size_t len, bool eof);

void gdma_out_start(int ch, gdma_desc_t *first);
void gdma_in_start(int ch, gdma_desc_t *first);
bool gdma_out_eof(int ch);                 /* OUT finished its EOF descriptor */
bool gdma_in_eof(int ch);                  /* IN got a successful EOF */
void gdma_stop(int ch);

/* Interrupt on every finished OUT descriptor (fn) / IN descriptor with EOF (fn).
 * fn(arg) runs at `level` on the calling core; NULL disables. */
int  gdma_on_out_eof(int ch, void (*fn)(void *), void *arg, int level);
int  gdma_on_in_done(int ch, void (*fn)(void *), void *arg, int level);
/* address of the descriptor the OUT/IN engine last finished (EOF bookkeeping) */
gdma_desc_t *gdma_out_eof_desc(int ch);
gdma_desc_t *gdma_in_eof_desc(int ch);

/* blocking memory copy through DMA channel ch (connected as GDMA_M2M) */
int  gdma_memcpy(int ch, void *dst, const void *src, size_t len);
/* Start a copy (<= 65 472 B) and return at once; the CPU keeps running.
 * done(arg) runs once in the channel's IN interrupt at `level` on the calling
 * core when the last byte is in dst (NULL: poll gdma_busy()). */
int  gdma_memcpy_async(int ch, void *dst, const void *src, size_t len, void (*done)(void *), void *arg, int level);
bool gdma_busy(int ch);
