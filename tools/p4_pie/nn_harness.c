/* Host-check harness: calls the nn.h layers with arguments from a parameter block, so the same inputs can run through the
 * PIE build and the portable build of components/lib/nn.c inside the model (tools/p4_pie/test_nn_layers.py). */
#include <stdint.h>
#include <stddef.h>
#include "nn.h"
#define P(i) ((void *)(uintptr_t)a[i])
void *memset(void *d, int c, size_t n) { uint8_t *p = d; while (n--) *p++ = (uint8_t)c; return d; }
void *memcpy(void *d, const void *s, size_t n) { uint8_t *p = d; const uint8_t *q = s; while (n--) *p++ = *q++; return d; }
void *memmove(void *d, const void *s, size_t n)
{
    uint8_t *p = d; const uint8_t *q = s;
    if (p < q) while (n--) *p++ = *q++; else { p += n; q += n; while (n--) *--p = *--q; }
    return d;
}
void t_conv(uint32_t *a) { nn_conv_s8(P(0), P(1), a[2], a[3], P(4), P(5), P(6), P(7), P(8), P(9), P(10)); }
void t_dw(uint32_t *a)   { nn_dwconv_s8(P(0), P(1), a[2], a[3], P(4), P(5), P(6), P(7), P(8), P(9), P(10)); }
void t_fc(uint32_t *a)   { nn_fc_s8(P(0), a[1], a[2], P(3), a[4], P(5), P(6), a[7], a[8], a[9], a[10], a[11], a[12]); }
void t_max(uint32_t *a)  { nn_maxpool_s8(P(0), a[1], a[2], P(3), a[4], a[5], a[6], a[7], a[8], a[9], a[10], a[11], a[12], a[13], a[14]); }
void t_avg(uint32_t *a)  { nn_avgpool_s8(P(0), a[1], a[2], P(3), a[4], a[5], a[6], a[7], a[8], a[9], a[10], a[11], a[12], a[13], a[14]); }
void t_add(uint32_t *a)  { nn_add_s8(P(0), P(1), a[2], a[3], a[4], a[5], a[6], a[7], a[8], P(9), a[10], a[11], a[12], a[13], a[14], a[15]); }
void t_relu6(uint32_t *a) { nn_relu6_s8(P(0), a[1]); }
