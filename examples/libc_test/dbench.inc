/* per-call cost of the double helpers through a function pointer, minus a dummy call */
extern double __adddf3(double, double), __muldf3(double, double), __divdf3(double, double), __subdf3(double, double);
static double __attribute__((noinline)) dummy2(double a, double b) { (void)b; return a; }
typedef double (*f2_t)(double, double);
static uint32_t dcost(f2_t f, double a, double b)
{
    volatile f2_t vf = f; f2_t g = vf; volatile double r;
    uint32_t best = ~0u;
    for (int k = 0; k < 5; k++) {
        uint32_t t = dcc();
        for (int i = 0; i < 64; i++) r = g(a, b);
        t = dcc() - t; if (t < best) best = t;
    }
    (void)r; return best;
}
static void dbench(void)
{
    static const struct { const char *n; f2_t f; double a, b; } c[] = {
        { "add same sign", __adddf3, 1.2345678901234, 6.789012345678 },
        { "add diff sign", __adddf3, 1.2345678901234, -6.789012345678 },
        { "add d = 33", __adddf3, 1.2345678901234e10, 1.0 },
        { "sub cancel", __subdf3, 1.0000001, 1.0 },
        { "mul", __muldf3, 1.2345678901234, 6.789012345678 },
        { "div", __divdf3, 1.2345678901234, 6.789012345678 },
    };
    uint32_t base = dcost(dummy2, 1.0, 2.0);
    for (unsigned i = 0; i < sizeof c / sizeof c[0]; i++)
        printf("     %-14s %lu cycles/call\n", c[i].n, (unsigned long)((dcost(c[i].f, c[i].a, c[i].b) - base) / 64));
}
