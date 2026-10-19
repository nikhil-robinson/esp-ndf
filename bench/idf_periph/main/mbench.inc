/* per-call cycles of math functions through function pointers (same file in the IDF sibling) */
#include <math.h>
typedef float (*mf1)(float); typedef float (*mf2)(float, float);
typedef double (*md1)(double); typedef double (*md2)(double, double);
static uint32_t __attribute__((noinline)) mcost_f1(mf1 f, float a)
{
    volatile mf1 vf = f; mf1 g = vf; volatile float r; uint32_t best = ~0u;
    for (int k = 0; k < 4; k++) { uint32_t t = mcc(); for (int i = 0; i < 16; i++) r = g(a); t = mcc() - t; if (t < best) best = t; }
    (void)r; return best / 16;
}
static uint32_t __attribute__((noinline)) mcost_f2(mf2 f, float a, float b)
{
    volatile mf2 vf = f; mf2 g = vf; volatile float r; uint32_t best = ~0u;
    for (int k = 0; k < 4; k++) { uint32_t t = mcc(); for (int i = 0; i < 16; i++) r = g(a, b); t = mcc() - t; if (t < best) best = t; }
    (void)r; return best / 16;
}
static uint32_t __attribute__((noinline)) mcost_d1(md1 f, double a)
{
    volatile md1 vf = f; md1 g = vf; volatile double r; uint32_t best = ~0u;
    for (int k = 0; k < 4; k++) { uint32_t t = mcc(); for (int i = 0; i < 16; i++) r = g(a); t = mcc() - t; if (t < best) best = t; }
    (void)r; return best / 16;
}
static uint32_t __attribute__((noinline)) mcost_d2(md2 f, double a, double b)
{
    volatile md2 vf = f; md2 g = vf; volatile double r; uint32_t best = ~0u;
    for (int k = 0; k < 4; k++) { uint32_t t = mcc(); for (int i = 0; i < 16; i++) r = g(a, b); t = mcc() - t; if (t < best) best = t; }
    (void)r; return best / 16;
}
static float idf1(float a) { return a; }
static double idd1(double a) { return a; }
#define F1(fn, a) printf("     %-10s(%-7s) %6lu\n", #fn, #a, (unsigned long)(mcost_f1(fn, a) - b1))
#define F2(fn, a, b) printf("     %-10s(%-7s) %6lu\n", #fn, #a "," #b, (unsigned long)(mcost_f2(fn, a, b) - b1))
#define D1(fn, a) printf("     %-10s(%-7s) %6lu\n", #fn, #a, (unsigned long)(mcost_d1(fn, a) - b2))
#define D2(fn, a, b) printf("     %-10s(%-7s) %6lu\n", #fn, #a "," #b, (unsigned long)(mcost_d2(fn, a, b) - b2))
static void mbench(void)
{
    uint32_t b1 = mcost_f1(idf1, 1.0f), b2 = mcost_d1(idd1, 1.0);
    printf("     math: cycles per call\n");
    F1(sinf, 0.7f); F1(sinf, 2.5f); F1(sinf, 1e4f); F1(cosf, 0.7f); F1(cosf, 2.5f); F1(tanf, 0.7f); F1(tanf, 2.5f);
    F1(asinf, 0.3f); F1(asinf, 0.8f); F1(acosf, 0.8f); F1(atanf, 0.7f); F1(atanf, 3.0f); F2(atan2f, 1.2f, -0.7f);
    F1(expf, 2.3f); F1(exp2f, 2.3f); F1(expm1f, 0.01f); F1(logf, 3.7f); F1(log2f, 3.7f); F1(log10f, 3.7f); F1(log1pf, 0.01f);
    F2(powf, 2.1f, 3.3f); F1(sqrtf, 2.0f); F1(cbrtf, 2.0f); F2(hypotf, 3.0f, 4.0f); F1(sinhf, 0.7f); F1(coshf, 0.7f); F1(tanhf, 0.7f);
    F1(floorf, 2.5f); F1(roundf, 2.5f); F2(fmodf, 10.3f, 3.0f);
#ifndef MB_NO_DOUBLE
    D1(sin, 0.7); D1(sin, 2.5); D1(sin, 1e4); D1(cos, 0.7); D1(tan, 0.7); D1(asin, 0.3); D1(acos, 0.8); D1(atan, 0.7); D2(atan2, 1.2, -0.7);
    D1(exp, 2.3); D1(exp2, 2.3); D1(expm1, 0.01); D1(log, 3.7); D1(log2, 3.7); D1(log10, 3.7); D1(log1p, 0.01);
    D2(pow, 2.1, 3.3); D1(sqrt, 2.0); D1(cbrt, 2.0); D2(hypot, 3.0, 4.0); D1(sinh, 0.7); D1(cosh, 0.7); D1(tanh, 0.7);
    D1(floor, 2.5); D1(round, 2.5); D2(fmod, 10.3, 3.0);
#endif
}
