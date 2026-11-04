/* Full interrupt cost per level, with an empty C handler, using CCOMPARE:
 *   entry  = handler's first CCOUNT - match time
 *   exit   = first CCOUNT back in the interrupted loop - handler's last CCOUNT
 *   stolen = time the interrupted loop lost
 * Plus a correctness torture: a hardware-loop (LOOP instruction) checksum in
 * the main code while interrupts fire every 600 cycles. */
#include "sys.h"
#include "irq.h"
#include "console.h"
#include "xtensa.h"

#define N 20000
typedef struct { volatile uint32_t target, t_in, t_out, done; } probe_t;
static probe_t pr[3];

#define CC_HANDLER(i)                                                      \
RAM_FN static void cc##i##_isr(void *arg)                                 \
{                                                                         \
    uint32_t in = ccount();                                               \
    probe_t *p = arg;                                                     \
    __asm__ volatile("wsr.ccompare" #i " %0" :: "r"(p->target - 1));     \
    p->t_in = in; p->done = 1;                                            \
    p->t_out = ccount();                                                  \
}
CC_HANDLER(0)
CC_HANDLER(1)
CC_HANDLER(2)

static volatile uint32_t storm_level, storm_count;
#define STORM(i)                                                           \
RAM_FN static void storm##i(void *arg)                                    \
{                                                                         \
    (void)arg;                                                            \
    __asm__ volatile("wsr.ccompare" #i " %0" :: "r"(ccount() + 600));    \
    storm_count++;                                                        \
}
STORM(0)
STORM(1)
STORM(2)

/* checksum with a real zero-overhead LOOP: interrupts landing inside it
 * must not disturb LBEG / LEND / LCOUNT */
__attribute__((noinline)) static uint32_t hw_loop_sum(const uint32_t *p, int n)
{
    uint32_t s = 0, w, k = 16777619u;
    __asm__ volatile(
        "loop   %[n], 1f\n"
        "l32i   %[w], %[p], 0\n"
        "addi   %[p], %[p], 4\n"
        "xor    %[s], %[s], %[w]\n"
        "mull   %[s], %[s], %[k]\n"
        "1:\n"
        : [s] "+r"(s), [p] "+r"(p), [w] "=&r"(w) : [n] "r"(n), [k] "r"(k) : "memory");
    return s;
}
static uint32_t data[256];

int main(void)
{
    static const int cpu_int[3] = { 6, 15, 16 }, level[3] = { 1, 3, 5 };
    irq_fn_t fn[3] = { cc0_isr, cc1_isr, cc2_isr }, sf[3] = { storm0, storm1, storm2 };
    printf("\n=== interrupt cost, empty C handler (cycles, 240 = 1 us) ===\n");
    for (int i = 0; i < 256; i++) data[i] = i * 2654435761u;
    uint32_t ref = hw_loop_sum(data, 256);
    for (int k = 0; k < 3; k++) {
        probe_t *p = &pr[k];
        uint32_t far = ccount() - 1;
        if (k == 0) __asm__ volatile("wsr.ccompare0 %0" :: "r"(far));
        if (k == 1) __asm__ volatile("wsr.ccompare1 %0" :: "r"(far));
        if (k == 2) __asm__ volatile("wsr.ccompare2 %0" :: "r"(far));
        irq_install(cpu_int[k], fn[k], p);
        irq_enable_all();
        uint32_t emin = ~0u, emax = 0, xmin = ~0u, xmax = 0, smin = ~0u, smax = 0;
        for (int i = 0; i < N; i++) {
            p->done = 0;
            p->target = ccount() + 2000;
            if (k == 0) __asm__ volatile("wsr.ccompare0 %0; rsync" :: "r"(p->target));
            if (k == 1) __asm__ volatile("wsr.ccompare1 %0; rsync" :: "r"(p->target));
            if (k == 2) __asm__ volatile("wsr.ccompare2 %0; rsync" :: "r"(p->target));
            uint32_t prev = ccount(), now;
            for (;;) { now = ccount(); if (p->done) break; prev = now; }
            uint32_t e = p->t_in - p->target, x = now - p->t_out, s = now - prev;
            if (e < emin) emin = e;
            if (e > emax) emax = e;
            if (x < xmin) xmin = x;
            if (x > xmax) xmax = x;
            if (s < smin) smin = s;
            if (s > smax) smax = s;
        }
        printf("  level %d: entry %3lu..%3lu  exit %3lu..%3lu  loop lost %3lu..%3lu\n", level[k], emin, emax, xmin, xmax, smin, smax);
        /* torture: interrupt storm at this level while the main loop uses LOOP */
        irq_install(cpu_int[k], sf[k], 0);
        storm_count = 0;
        uint32_t start = ccount() + 600;
        if (k == 0) __asm__ volatile("wsr.ccompare0 %0; rsync" :: "r"(start));
        if (k == 1) __asm__ volatile("wsr.ccompare1 %0; rsync" :: "r"(start));
        if (k == 2) __asm__ volatile("wsr.ccompare2 %0; rsync" :: "r"(start));
        int bad = 0;
        for (int r = 0; r < 20000; r++) bad += hw_loop_sum(data, 256) != ref;
        __asm__ volatile("rsil a2, 15" ::: "a2");     /* stop the storm */
        uint32_t f = ccount() - 1;
        if (k == 0) __asm__ volatile("wsr.ccompare0 %0" :: "r"(f));
        if (k == 1) __asm__ volatile("wsr.ccompare1 %0" :: "r"(f));
        if (k == 2) __asm__ volatile("wsr.ccompare2 %0" :: "r"(f));
        irq_enable_all();
        printf("           storm: %lu interrupts during 20000 hardware-loop checksums, %d wrong\n", storm_count, bad);
    }
    printf("=== done ===\n");
    for (;;) ;
}
