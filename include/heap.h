/* Heap: internal DRAM between the core-1 stack and the ROM loader area, plus PSRAM after heap_add_psram().
 * 16-byte aligned blocks (vector unit and DMA ready), first fit, neighbours
 * merged on free. Safe from both cores and from ISRs (short critical section).
 * Standard names, so ported code links unchanged. */
#pragma once
#include "ndf.h"

void  *malloc(size_t n);
void  *calloc(size_t n, size_t size);
void  *realloc(void *p, size_t n);
void   free(void *p);
size_t heap_free(void);          /* bytes free in total */
size_t heap_largest(void);       /* largest single allocation possible now */
size_t heap_min_free(void);      /* lowest heap_free() seen since boot */

/* capabilities (IDF heap_caps names where they exist) */
#define HEAP_INTERNAL 1u                 /* internal SRAM */
#define HEAP_DMA      2u                 /* reachable by GDMA (internal) */
#define HEAP_SPIRAM   4u                 /* external PSRAM */
void  *heap_caps_malloc(size_t n, uint32_t caps);   /* free() releases it */
size_t heap_caps_free_size(uint32_t caps);
int    heap_add_psram(void *start, size_t size);    /* after psram_init: PSRAM_VADDR, psram_size() */
