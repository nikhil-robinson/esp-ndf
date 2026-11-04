/* Fast math: accuracy (vs double precision over sweeps) and cycles per call vs our libm (musl, correctly rounded
 * float paths) and, in bench/idf_math, IDF's newlib. Q15/Q31 helpers checked on edge values. */
#include "console.h"
#include "fastmath.h"
#include "sys.h"
#include "cpu.h"
#include <math.h>
#include "fastmath_bench.h"

#define TIME(label, expr) do { uint32_t t = ccount(); for (int i = 0; i < FM_N; i++) fm_o[i] = (expr); t = ccount() - t; \
    printf("ndf %-14s %5lu.%lu cycles/call\n", label, (unsigned long)(t / FM_N), (unsigned long)(t % FM_N * 10 / FM_N)); } while (0)

int main(void)
{
    delay_ms(200);
    fm_args();
    double es = 0, ea = 0, ee = 0, el = 0, er = 0;
    for (int i = 0; i < 20000; i++) {
        float x = -8192 + 16384.0f * i / 20000; float s, c; fm_sincosf(x, &s, &c);
        es = fmax(es, fmax(fabs(s - sin((double)x)), fabs(c - cos((double)x))));
        float yy = -50 + 100.0f * i / 20000, xx = -3 + 6.0f * ((i * 7919) % 20000) / 20000;
        ea = fmax(ea, fabs(fm_atan2f(yy, xx) - atan2((double)yy, (double)xx)));
        float xe = -87 + 175.0f * i / 20000; double r = exp((double)xe); ee = fmax(ee, fabs(fm_expf(xe) - r) / r);
        float xl = 0.37f + 2.3f * (i + 1) / 20000; el = fmax(el, fabs(fm_logf(xl) - log((double)xl)));
        float z = 1e-6f * powf(10, 12.0f * (i + 1) / 20000); er = fmax(er, fabs(fm_sqrtf(z) - sqrt((double)z)) / sqrt((double)z));
    }
    int ok = es <= 1.2e-7 && ea <= 3e-7 && ee <= 2e-7 && el <= 2e-7 && er <= 2e-7;
    int qok = q15_mul(-32768, -32768) == 32767 && q15_mul(16384, 16384) == 8192 && q15_add(30000, 30000) == 32767 &&
              q31_mul(INT32_MIN, INT32_MIN) == INT32_MAX && q31_mul(1 << 30, 1 << 30) == 1 << 29 && q15_from_f(-1.0f) == -32768 &&
              q15_from_f(0.99999f) == 32767 && q31_add(INT32_MAX, 1) == INT32_MAX;
    printf("%s accuracy: sin/cos %.2e, atan2 %.2e, exp rel %.2e, log %.2e, sqrt rel %.2e; Q15/Q31 %s\n", ok && qok ? "PASS" : "FAIL",
           es, ea, ee, el, er, qok ? "ok" : "WRONG");
    TIME("sinf libm", sinf(fm_x[i]));
    TIME("sinf fast", fm_sinf(fm_x[i]));
    TIME("cosf libm", cosf(fm_x[i]));
    TIME("cosf fast", fm_cosf(fm_x[i]));
    TIME("atan2f libm", atan2f(fm_y[i], fm_x[i]));
    TIME("atan2f fast", fm_atan2f(fm_y[i], fm_x[i]));
    TIME("expf libm", expf(fm_x[i]));
    TIME("expf fast", fm_expf(fm_x[i]));
    TIME("logf libm", logf(fm_x[i] + 10.5f));
    TIME("logf fast", fm_logf(fm_x[i] + 10.5f));
    TIME("sqrtf libm", sqrtf(fm_x[i] + 10.5f));
    TIME("sqrtf fast", fm_sqrtf(fm_x[i] + 10.5f));
    { uint32_t t = ccount(); fm_sinf_v(fm_x, (float *)fm_o, FM_N); t = ccount() - t; printf("ndf %-14s %5lu.%lu cycles/call\n", "sinf fast vec", (unsigned long)(t / FM_N), (unsigned long)(t % FM_N * 10 / FM_N)); }
    { uint32_t t = ccount(); fm_cosf_v(fm_x, (float *)fm_o, FM_N); t = ccount() - t; printf("ndf %-14s %5lu.%lu cycles/call\n", "cosf fast vec", (unsigned long)(t / FM_N), (unsigned long)(t % FM_N * 10 / FM_N)); }
    { uint32_t t = ccount(); fm_expf_v(fm_x, (float *)fm_o, FM_N); t = ccount() - t; printf("ndf %-14s %5lu.%lu cycles/call\n", "expf fast vec", (unsigned long)(t / FM_N), (unsigned long)(t % FM_N * 10 / FM_N)); }
    printf("fastmath_test %s done\n", ok && qok ? "PASS" : "FAIL");
    for (;;) {}
}
