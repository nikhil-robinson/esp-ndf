/* SIMD / FPU kernels vs plain C (-O2), cycles per call, results checked. */
#include "sys.h"
#include "dsp.h"
#include "gdma.h"
#include "console.h"
#include "cpu.h"
#include <string.h>

#define N 1024
static int16_t a16[N] DSP_ALIGN, b16[N] DSP_ALIGN, o16[N] DSP_ALIGN, r16[N] DSP_ALIGN;
static float af[N] DSP_ALIGN, bf[N] DSP_ALIGN;
static uint8_t src[16384] DSP_ALIGN, dst[16384] DSP_ALIGN;

__attribute__((noinline)) static int32_t c_dot_s16(const int16_t *a, const int16_t *b, int n)
{ int32_t s = 0; for (int i = 0; i < n; i++) s += a[i] * b[i]; return s; }
__attribute__((noinline)) static void c_add_s16_sat(int16_t *d, const int16_t *a, const int16_t *b, int n)
{ for (int i = 0; i < n; i++) { int32_t v = a[i] + b[i]; d[i] = v > 32767 ? 32767 : v < -32768 ? -32768 : v; } }
__attribute__((noinline)) static float c_dot_f32(const float *a, const float *b, int n)
{ float s = 0; for (int i = 0; i < n; i++) s += a[i] * b[i]; return s; }

#define TIME(expr) ({ uint32_t _t = ccount(); expr; ccount() - _t; })

int main(void)
{
    printf("\n=== SIMD / FPU kernels vs C -O2 (cycles per call, 240 = 1 us) ===\n");
    uint32_t x = 12345;
    for (int i = 0; i < N; i++) {
        x = x * 1103515245u + 12345u; a16[i] = (int16_t)(x >> 16) / 8;
        x = x * 1103515245u + 12345u; b16[i] = (int16_t)(x >> 16) / 8;
        af[i] = (float)a16[i] * 0.001f; bf[i] = (float)b16[i] * 0.001f;
    }
    for (int i = 0; i < 16384; i++) src[i] = i * 7;

    int32_t rc = 0, rs = 0;
    uint32_t tc = TIME(rc = c_dot_s16(a16, b16, N)), ts = TIME(rs = dsp_dot_s16(a16, b16, N));
    printf("  dot product int16 x%d:   C %6lu  SIMD %6lu  x%lu.%02lu  %s\n", N, tc, ts, tc / ts, tc * 100 / ts % 100, rc == rs ? "equal" : "MISMATCH");

    /* saturating add with values that overflow */
    for (int i = 0; i < N; i++) { r16[i] = a16[i] * 6; o16[i] = 0; }
    tc = TIME(c_add_s16_sat(o16, r16, b16, N));
    static int16_t o2[N] DSP_ALIGN;
    ts = TIME(dsp_add_s16_sat(o2, r16, b16, N));
    printf("  saturating add int16 x%d: C %6lu  SIMD %6lu  x%lu.%02lu  %s\n", N, tc, ts, tc / ts, tc * 100 / ts % 100, memcmp(o16, o2, sizeof o2) ? "MISMATCH" : "equal");

    float fc = 0, fs = 0;
    tc = TIME(fc = c_dot_f32(af, bf, N)); ts = TIME(fs = dsp_dot_f32(af, bf, N));
    float diff = fc - fs; if (diff < 0) diff = -diff;
    float mag = fc < 0 ? -fc : fc;
    printf("  dot product float x%d:   C %6lu  FPU  %6lu  x%lu.%02lu  %s\n", N, tc, ts, tc / ts, tc * 100 / ts % 100,
           diff <= mag * 1e-5f + 1e-3f ? "equal (within float rounding)" : "MISMATCH");

    tc = TIME(memcpy(dst, src, sizeof src));
    memset(dst, 0, sizeof dst);
    ts = TIME(dsp_copy128(dst, src, sizeof src));
    int ch = gdma_alloc(); gdma_connect(ch, GDMA_M2M);
    static uint8_t dst2[16384] DSP_ALIGN;
    uint32_t tg = TIME(gdma_memcpy(ch, dst2, src, sizeof src));
    printf("  copy 16 KB: memcpy %lu (%lu MB/s)  SIMD %lu (%lu MB/s)  GDMA %lu (%lu MB/s)  %s\n",
           tc, 16384u * (sys_cpu_hz() / 1000000) / tc, ts, 16384u * (sys_cpu_hz() / 1000000) / ts, tg, 16384u * (sys_cpu_hz() / 1000000) / tg,
           memcmp(src, dst, sizeof src) || memcmp(src, dst2, sizeof src) ? "MISMATCH" : "equal");
    /* every length 16..512 (step 16): pipelined prologue/epilogue, saturation */
    int bad = 0;
    for (int n = 16; n <= 512; n += 16) {
        if (c_dot_s16(a16, b16, n) != dsp_dot_s16(a16, b16, n)) bad++;
        for (int i = 0; i < n; i++) r16[i] = a16[i] * 6;               /* overflows: exercises saturation */
        static int16_t x1[N] DSP_ALIGN, x2[N] DSP_ALIGN;
        x1[n] = x2[n] = 0x7777;                                         /* guard: nothing written past n */
        c_add_s16_sat(x1, r16, b16, n); dsp_add_s16_sat(x2, r16, b16, n);
        if (memcmp(x1, x2, n * 2) || x2[n] != 0x7777) bad++;
    }
    printf("  lengths 16..512: %d mismatches\n", bad);
    printf("=== done ===\n");
    for (;;) ;
}
