/* Heap: internal DRAM between the core-1 stack and the ROM loader area.
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
