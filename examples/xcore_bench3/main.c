/* Which memory-ordering method is cheapest AND still correct on the S3?
 * V1 memw barriers, V2 acquire/release (L32AI/S32RI), V3 compiler barrier
 * only (relies on the in-order core; IDF's spinlock release does the same).
 * Each variant: loop cost (both cores running) + torture tests. */
#include "xcore.h"
#include "sys.h"
#include "console.h"

#define CB()  __asm__ volatile("" ::: "memory")
static inline void st_rel(volatile uint32_t *p, uint32_t v) { __asm__ volatile("s32ri %0, %1, 0" :: "r"(v), "r"(p) : "memory"); }
static inline uint32_t ld_acq(volatile uint32_t *p) { uint32_t v; __asm__ volatile("l32ai %0, %1, 0" : "=r"(v) : "r"(p) : "memory"); return v; }

/* sequence counter, 3 ways */
static volatile uint32_t seq;
static uint32_t data[16];
static int V;                                   /* 1 memw, 2 acq/rel, 3 none */

static inline void wr(uint32_t val, int n)
{
    uint32_t s = seq;
    if (V == 1) { seq = s + 1; xc_barrier(); for (int k = 0; k < n; k++) data[k] = val; xc_barrier(); seq = s + 2; }
    else if (V == 2) { seq = s + 1; CB(); for (int k = 0; k < n; k++) data[k] = val; st_rel(&seq, s + 2); }
    else { seq = s + 1; CB(); for (int k = 0; k < n; k++) data[k] = val; CB(); seq = s + 2; }
}
static inline bool rd(uint32_t *out, int n)     /* false: torn copy detected by content */
{
    uint32_t a, b;
    do {
        do a = V == 2 ? ld_acq(&seq) : seq; while (a & 1);
        if (V == 1) xc_barrier(); else CB();
        for (int k = 0; k < n; k++) out[k] = data[k];
        if (V == 1) xc_barrier(); else CB();
        b = V == 2 ? ld_acq(&seq) : seq;
    } while (a != b);
    for (int k = 1; k < n; k++) if (out[k] != out[0]) return false;
    return true;
}

/* spinlock with 3 kinds of unlock */
static xc_spin_t lk;
static inline void unlock(void)
{
    if (V == 1) xc_spin_unlock(&lk);
    else if (V == 2) st_rel(&lk.owner, 0);
    else { CB(); lk.owner = 0; }
}
static struct { uint32_t a, b, n; } crit;

static volatile uint32_t phase, go, done1, r_torn, r_reads, r_bad;
enum { P_COST16 = 1, P_COST64, P_TORN, P_LOCK };
static inline uint32_t work(uint32_t x) { for (int k = 0; k < 40; k++) { x = x * 1103515245u + 12345u; __asm__ volatile("" : "+r"(x)); } return x; }
static volatile uint32_t sink;
static uint32_t c1_avg, c1_max;

static void core1(void)
{
    uint32_t last = 0, buf[16];
    for (;;) {
        while (go == last) ;
        last = go;
        uint32_t p = phase;
        if (p == P_COST16 || p == P_COST64) {
            int n = p == P_COST16 ? 4 : 16; uint32_t x = 1, sum = 0, mx = 0;
            for (int i = 0; i < 20000; i++) { uint32_t t = ccount(); x = work(x); rd(buf, n); x ^= buf[0]; t = ccount() - t; sum += t; if (t > mx) mx = t; }
            c1_avg = sum / 20000; c1_max = mx; sink = x;
        } else if (p == P_TORN) {
            uint32_t torn = 0, reads = 0;
            while (phase == P_TORN) { torn += !rd(buf, 16); reads++; }
            r_torn = torn; r_reads = reads;
        } else if (p == P_LOCK) {
            for (int i = 0; i < 200000; i++) { xc_spin_lock(&lk); crit.a++; crit.b += crit.a; crit.n++; unlock(); }
        }
        done1 = last;
    }
}
static void start(uint32_t p) { phase = p; done1 = 0; go = go + 1; }
static void wait1(void) { while (done1 != go) ; }

int main(void)
{
    printf("\n=== xcore_bench3: memory ordering, cost vs correctness (cycles) ===\n");
    cpu1_start(core1);
    delay_us(10000);
    static const char *const vn[] = { "", "V1 memw", "V2 acquire/release", "V3 compiler only" };
    /* baseline loop without any sharing */
    { uint32_t x = 1, sum = 0; for (int i = 0; i < 20000; i++) { uint32_t t = ccount(); x = work(x); sum += ccount() - t; } sink = x; printf("  baseline loop: %lu cycles\n", sum / 20000); }
    for (V = 1; V <= 3; V++) {
        printf("\n  %s\n", vn[V]);
        for (int n = 4; n <= 16; n += 12) {
            start(n == 4 ? P_COST16 : P_COST64);
            uint32_t x = 1, sum = 0, mx = 0;
            for (uint32_t i = 1; i <= 20000; i++) { uint32_t t = ccount(); x = work(x); wr(i, n); t = ccount() - t; sum += t; if (t > mx) mx = t; }
            wait1(); sink = x;
            printf("    %2d B struct: writer loop avg %lu max %lu | reader loop avg %lu max %lu\n", n * 4, sum / 20000, mx, c1_avg, c1_max);
        }
        /* torture 1: 300 ms each, reader copies 64 B; writer back to back, then paced */
        static const uint32_t pace[] = { 0, 300, 1000 };
        for (int k = 0; k < 3; k++) {
            start(P_TORN);
            uint32_t t0 = ccount(), w = 0;
            while (ccount() - t0 < 72000000u) { uint32_t tw = ccount(); wr(++w, 16); if (pace[k]) while (ccount() - tw < pace[k]) ; }
            phase = 0; wait1();
            printf("    writer every %4lu cycles: %7lu writes, reader %7lu reads, torn %lu\n", pace[k] ? pace[k] : 0, w, r_reads, r_torn);
        }
        /* torture 2: both cores hammer one lock, 200 000 times each */
        crit = (typeof(crit)){ 0 };
        start(P_LOCK);
        uint32_t t1 = ccount();
        for (int i = 0; i < 200000; i++) { xc_spin_lock(&lk); crit.a++; crit.b += crit.a; crit.n++; unlock(); }
        t1 = ccount() - t1; wait1();
        printf("    lock torture: n %lu a %lu (expect 400000 both) %s, %lu cycles/op contended\n",
               crit.n, crit.a, crit.n == 400000 && crit.a == 400000 ? "ok" : "LOST UPDATES", t1 / 200000);
        uint32_t tu = ccount(); xc_spin_lock(&lk); unlock(); tu = ccount() - tu;
        printf("    lock+unlock uncontended: %lu cycles\n", tu);
    }
    printf("=== done ===\n");
    for (;;) ;
}
