/* Cost of sharing data between the two cores, in CPU cycles (240 = 1 us).
 * Core 0 measures round trips: ping -> core 1 answers -> pong. One-way is
 * about half. Also checks correctness: torn reads, lost lock updates. */
#include "xcore.h"
#include "sys.h"
#include "console.h"

#define N 10000
/* a reader copying a bigger struct: ~40 cycles between its first and last load */
#define SLOW() __asm__ volatile("nop; nop; nop; nop; nop; nop; nop; nop; nop; nop; nop; nop; nop; nop; nop; nop; nop; nop; nop; nop")


enum { T_IDLE, T_PLAIN, T_SEQ, T_SPIN_RT, T_SPIN_HAMMER, T_TORN, T_NOTIFY, T_DONE };
static volatile uint32_t mode, ack, started;                /* test control */
static volatile uint32_t ping, pong;               /* 1. plain 32-bit words */
static struct { xc_seq_t s; uint32_t a, b, c, d; } shared;   /* 2. struct + seq counter */
static struct { volatile uint32_t a, b, c, d; } unprot;      /* same, no protection */
static xc_spin_t lock;
static volatile uint32_t locked_ctr, torn_seq, torn_plain, reads;

static void notify_handler(void *arg) { (void)arg; pong = ping; }

static volatile int notify_ok = 99;
static void core1(void)
{
    notify_ok = xc_notify_init(notify_handler, 0, 1);
    uint32_t seen = 0;
    for (;;) {
        uint32_t m = mode;
        if (m == T_PLAIN) {                         /* echo ping into pong */
            uint32_t p = ping;
            if (p != seen) { seen = p; pong = p; }
        } else if (m == T_SEQ) {                    /* read the struct, echo its value */
            uint32_t v, a, b, c, d;
            do { v = xc_seq_read_begin(&shared.s); a = shared.a; b = shared.b; c = shared.c; d = shared.d; }
            while (xc_seq_read_retry(&shared.s, v));
            if (a != seen) { if (a != b || b != c || c != d) torn_seq++; seen = a; pong = a; }
        } else if (m == T_SPIN_RT) {                /* echo under the lock */
            xc_spin_lock(&lock);
            uint32_t p = ping;
            if (p != seen) { seen = p; pong = p; }
            xc_spin_unlock(&lock);
        } else if (m == T_SPIN_HAMMER) {
            for (int i = 0; i < N * 10; i++) { xc_spin_lock(&lock); locked_ctr++; xc_spin_unlock(&lock); }
            ack = 1;
            while (mode == T_SPIN_HAMMER) ;
        } else if (m == T_TORN) {                   /* read both versions while core 0 writes */
            uint32_t ts = 0, tp = 0, r = 0;
            started = 1;
            while (mode == T_TORN) {
                uint32_t v, a, b, c, d;
                do { v = xc_seq_read_begin(&shared.s); a = shared.a; b = shared.b; SLOW(); c = shared.c; d = shared.d; }
                while (xc_seq_read_retry(&shared.s, v));
                ts += a != b || b != c || c != d;
                a = unprot.a; b = unprot.b; SLOW(); c = unprot.c; d = unprot.d;
                tp += a != b || b != c || c != d;
                r++;
            }
            torn_seq = ts; torn_plain = tp; reads = r; ack = 1;
        }
    }
}

typedef struct { uint32_t min, max; uint64_t sum; } stat_t;
static void st_add(stat_t *s, uint32_t v) { if (v < s->min) s->min = v; if (v > s->max) s->max = v; s->sum += v; }
static void st_print(const char *name, stat_t *s, int n)
{
    printf("  %-40s min %4lu  avg %4lu  max %4lu cycles\n", name, s->min, (uint32_t)(s->sum / n), s->max);
}

int main(void)
{
    printf("\n=== xcore_bench: core 0 <-> core 1, round trips ===\n");
    cpu1_start(core1);
    delay_us(10000);

    /* single-core costs (no other core involved) */
    uint32_t t, c0;
    t = ccount(); xc_seq_write_begin(&shared.s); shared.a = 1; shared.b = 1; shared.c = 1; shared.d = 1; xc_seq_write_end(&shared.s); c0 = ccount() - t;
    printf("  seq-counter write of 4 words (one core)     %lu cycles (plain 4 stores: ", c0);
    t = ccount(); unprot.a = 1; unprot.b = 1; unprot.c = 1; unprot.d = 1; printf("%lu)\n", ccount() - t);
    t = ccount(); xc_spin_lock(&lock); xc_spin_unlock(&lock); c0 = ccount() - t;
    printf("  spinlock lock+unlock, uncontended           %lu cycles\n", c0);
    t = ccount(); xc_barrier(); printf("  memw barrier                                %lu cycles\n", ccount() - t);

    stat_t s;
    /* 1. plain word */
    mode = T_PLAIN; delay_us(100);
    s = (stat_t){ ~0u, 0, 0 };
    for (uint32_t i = 1; i <= N; i++) { t = ccount(); ping = i; while (pong != i) ; st_add(&s, ccount() - t); }
    st_print("plain 32-bit word, round trip", &s, N);

    /* 2. seq-counter struct */
    mode = T_SEQ; delay_us(100); torn_seq = 0;
    s = (stat_t){ ~0u, 0, 0 };
    for (uint32_t i = 1; i <= N; i++) {
        t = ccount();
        xc_seq_write_begin(&shared.s); shared.a = i; shared.b = i; shared.c = i; shared.d = i; xc_seq_write_end(&shared.s);
        while (pong != i) ;
        st_add(&s, ccount() - t);
    }
    st_print("seq-counter 16-byte struct, round trip", &s, N);

    /* 3. spinlock-protected word */
    mode = T_SPIN_RT; delay_us(100);
    s = (stat_t){ ~0u, 0, 0 };
    for (uint32_t i = N + 1; i <= 2 * N; i++) {
        t = ccount(); xc_spin_lock(&lock); ping = i; xc_spin_unlock(&lock); while (pong != i) ; st_add(&s, ccount() - t);
    }
    st_print("spinlock-protected word, round trip", &s, N);

    /* 4. cross-core interrupt */
    mode = T_NOTIFY; delay_us(100);
    s = (stat_t){ ~0u, 0, 0 };
    if (notify_ok) printf("  core 1 notify init failed: %d\n", notify_ok);
    for (uint32_t i = 2 * N + 1; i <= 3 * N; i++) {
        ping = i; t = ccount(); xc_notify(1);
        uint32_t t0 = ccount(); while (pong != i && ccount() - t0 < 2400000) ;
        if (pong != i) { printf("  notify: no answer at %lu\n", i); break; }
        st_add(&s, ccount() - t);
    }
    st_print("cross-core interrupt, round trip", &s, N);

    /* 5. contended spinlock: both cores increment one counter */
    locked_ctr = 0; ack = 0; mode = T_SPIN_HAMMER;
    t = ccount();
    for (int i = 0; i < N * 10; i++) { xc_spin_lock(&lock); locked_ctr++; xc_spin_unlock(&lock); }
    uint32_t mine = ccount() - t;
    while (!ack) ;
    mode = T_IDLE;
    printf("  spinlock, both cores hammering: counter %lu (expect %u), %lu cycles per lock+inc+unlock on core 0\n",
           locked_ctr, 2 * N * 10, mine / (N * 10));

    /* 6. torn reads: (a) writer back to back, (b) writer at 1 MHz (every 240 cycles) */
    for (int phase = 0; phase < 2; phase++) {
        ack = 0; started = 0; mode = T_TORN;
        while (!started) ;
        uint32_t t_end = ccount();
        for (uint32_t i = 1; ccount() - t_end < 24000000u; i++) {      /* 100 ms */
            uint32_t tw = ccount();
            xc_seq_write_begin(&shared.s); shared.a = i; shared.b = i; shared.c = i; shared.d = i; xc_seq_write_end(&shared.s);
            unprot.a = i; unprot.b = i; unprot.c = i; unprot.d = i;
            if (phase) while (ccount() - tw < 240) ;
        }
        mode = T_IDLE; while (!ack) ;
        printf("  100 ms, writer %s: seq-counter reads %lu, torn %lu | unprotected torn %lu of %lu\n",
               phase ? "every 240 cycles (1 MHz)" : "back to back (~30 cycles)", reads, torn_seq, torn_plain, reads);
    }
    printf("=== done ===\n");
    for (;;) ;
}
