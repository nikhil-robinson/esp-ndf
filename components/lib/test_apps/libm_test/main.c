/* Math library accuracy + speed. Inputs come from a xorshift PRNG that
 * tools/libm_check.py reproduces; results go out in hex and the host computes
 * the exact error (mpmath) per function. Speed: same table as the IDF sibling. */
#include "console.h"
#include "cpu.h"
#include "sys.h"
#include <math.h>
#include <stdint.h>
#include <string.h>
float exp10f(float);
static inline uint32_t mcc(void) { return ccount(); }
#define MB_NO_DOUBLE
#include "../libc_test/mbench.inc"

static uint32_t st;
static uint32_t rnd(void) { uint32_t x = st; x ^= x << 13; x ^= x >> 17; x ^= x << 5; return st = x; }
/* float from sign/exponent ranges: exponent uniform in [emin, emax], mantissa random */
static float gen(int emin, int emax, int neg)
{
    uint32_t e = (uint32_t)(emin + (int)(rnd() % (uint32_t)(emax - emin + 1)) + 127);
    uint32_t u = e << 23 | (rnd() & 0x7FFFFF);
    if (neg && (rnd() & 1)) u |= 0x80000000;
    union { uint32_t u; float f; } v = { u }; return v.f;
}
static float bounded(float lo, float hi) { return lo + (hi - lo) * (float)(rnd() >> 8) * 0x1p-24f; }

typedef struct { const char *name; float (*f1)(float); float (*f2)(float, float); int emin, emax, neg; float lo, hi; int ylo, yhi; } fn_t;
#define N 1500
static const fn_t fns[] = {
    { .name = "sinf", .f1 = sinf, .emin = -20, .emax = 8, .neg = 1 }, { .name = "sinf_big", .f1 = sinf, .emin = 9, .emax = 127, .neg = 1 }, { .name = "cosf", .f1 = cosf, .emin = -20, .emax = 8, .neg = 1 }, { .name = "cosf_big", .f1 = cosf, .emin = 9, .emax = 127, .neg = 1 },
    { .name = "tanf", .f1 = tanf, .emin = -20, .emax = 8, .neg = 1 }, { .name = "tanf_big", .f1 = tanf, .emin = 9, .emax = 127, .neg = 1 },
    { .name = "asinf", .f1 = asinf, .emin = 0, .emax = 0, .neg = 1, .lo = -1.0f, .hi = 1.0f }, { .name = "acosf", .f1 = acosf, .emin = 0, .emax = 0, .neg = 1, .lo = -1.0f, .hi = 1.0f },
    { .name = "atanf", .f1 = atanf, .emin = -20, .emax = 30, .neg = 1 }, { .name = "atan2f", .f2 = atan2f, .emin = -20, .emax = 20, .neg = 1, .ylo = -20, .yhi = 20 },
    { .name = "expf", .f1 = expf, .emin = 0, .emax = 0, .neg = 1, .lo = -103.0f, .hi = 88.7f }, { .name = "exp2f", .f1 = exp2f, .emin = 0, .emax = 0, .neg = 1, .lo = -149.0f, .hi = 127.9f }, { .name = "exp10f", .f1 = exp10f, .emin = 0, .emax = 0, .neg = 1, .lo = -45.0f, .hi = 38.5f },
    { .name = "expm1f", .f1 = expm1f, .emin = -26, .emax = 6, .neg = 1 }, { .name = "logf", .f1 = logf, .emin = -126, .emax = 127 }, { .name = "log2f", .f1 = log2f, .emin = -126, .emax = 127 }, { .name = "log10f", .f1 = log10f, .emin = -126, .emax = 127 },
    { .name = "log1pf", .f1 = log1pf, .emin = -26, .emax = 30 }, { .name = "logf_near1", .f1 = logf, .emin = 0, .emax = 0, .lo = 0.9f, .hi = 1.1f },
    { .name = "powf", .f2 = powf, .emin = 0, .emax = 0, .lo = 0.01f, .hi = 100.0f, .ylo = -8, .yhi = 4 }, { .name = "cbrtf", .f1 = cbrtf, .emin = -126, .emax = 127, .neg = 1 }, { .name = "hypotf", .f2 = hypotf, .emin = -60, .emax = 60, .neg = 1, .ylo = -60, .yhi = 60 },
    { .name = "sinhf", .f1 = sinhf, .emin = -20, .emax = 6, .neg = 1 }, { .name = "coshf", .f1 = coshf, .emin = -20, .emax = 6, .neg = 1 }, { .name = "tanhf", .f1 = tanhf, .emin = -20, .emax = 4, .neg = 1 },
    { .name = "asinhf", .f1 = asinhf, .emin = -20, .emax = 40, .neg = 1 }, { .name = "acoshf", .f1 = acoshf, .emin = 0, .emax = 0, .lo = 1.0f, .hi = 50.0f }, { .name = "atanhf", .f1 = atanhf, .emin = 0, .emax = 0, .neg = 1, .lo = -0.999f, .hi = 0.999f },
    { .name = "sqrtf", .f1 = sqrtf, .emin = -126, .emax = 127 }, { .name = "fmodf", .f2 = fmodf, .emin = -20, .emax = 30, .neg = 1, .ylo = -20, .yhi = 20 },
};

int main(void)
{
    delay_ms(1500);                                   /* host attaches */
    printf("libm_test\n");
    for (unsigned k = 0; k < sizeof fns / sizeof fns[0]; k++) {
        const fn_t *f = &fns[k];
        st = 0x9E3779B9u ^ (k * 0x01000193u);
        printf("@%s %d\n", f->name, N);
        char line[9 * 8 + 2]; int lp = 0;
        for (int i = 0; i < N; i++) {
            float x = f->lo != f->hi ? bounded(f->lo, f->hi) : gen(f->emin, f->emax, f->neg), r;
            if (f->f2) {
                float y = f->ylo != f->yhi ? (!strcmp(f->name, "powf") ? bounded((float)f->ylo, (float)f->yhi) : gen(f->ylo, f->yhi, 1)) : 0;
                r = f->f2(x, y);
            } else r = f->f1(x);
            union { float f; uint32_t u; } v = { r };
            static const char hx[] = "0123456789abcdef";
            for (int b = 7; b >= 0; b--) line[lp++] = hx[(v.u >> (4 * b)) & 15];
            line[lp++] = ' ';
            if (lp >= 9 * 8) { line[lp++] = '\n'; line[lp] = 0; printf("%s", line); lp = 0; }
        }
        if (lp) { line[lp++] = '\n'; line[lp] = 0; printf("%s", line); }
    }
    mbench();
    printf("=== done\n");
    for (;;) {}
}
