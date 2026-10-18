/* Cross-core design benchmark: loose coupling vs locks vs Remote Core Locking
 * vs shared FIFO, plus SRAM bank contention. Both cores run each phase at the
 * same time; all numbers are CPU cycles (240 = 1 us). */
#include "xcore.h"
#include "sys.h"
#include "console.h"

#define ITERS 20000
static volatile uint32_t phase, go, done1;

/* ---- fixed, deterministic work standing in for a control-loop body ------ */
static inline uint32_t work(uint32_t x)
{
    for (int k = 0; k < 40; k++) { x = x * 1103515245u + 12345u; __asm__ volatile("" : "+r"(x)); }
    return x;
}

typedef struct { uint32_t min, max; uint64_t sum; uint32_t n; } stat_t;
static void st_reset(stat_t *s) { *s = (stat_t){ ~0u, 0, 0, 0 }; }
static void st_add(stat_t *s, uint32_t v) { if (v < s->min) s->min = v; if (v > s->max) s->max = v; s->sum += v; s->n++; }
static uint32_t st_avg(stat_t *s) { return s->n ? (uint32_t)(s->sum / s->n) : 0; }

/* ---- shared data -------------------------------------------------------- */
static volatile uint32_t word;
static struct { xc_seq_t s; uint32_t v[4]; } s16;
static struct { xc_seq_t s; uint32_t v[16]; } s64;
static struct { uint32_t v[4]; } locked16;
static xc_spin_t lk;
static stat_t st1;                              /* core 1 results */
static volatile uint32_t sink;

enum { P_BASE = 1, P_PLAIN, P_SEQ16, P_SEQ64, P_SPIN16 };

/* one loop iteration of core c in a coupling phase */
static inline uint32_t couple_iter(int c, uint32_t p, uint32_t i, uint32_t x)
{
    x = work(x);
    if (p == P_PLAIN) { if (c == 0) word = i; else x ^= word; }
    else if (p == P_SEQ16) {
        if (c == 0) { xc_seq_write_begin(&s16.s); for (int k = 0; k < 4; k++) s16.v[k] = i; xc_seq_write_end(&s16.s); }
        else { uint32_t v, a[4]; do { v = xc_seq_read_begin(&s16.s); for (int k = 0; k < 4; k++) a[k] = s16.v[k]; } while (xc_seq_read_retry(&s16.s, v)); x ^= a[0] ^ a[3]; }
    } else if (p == P_SEQ64) {
        if (c == 0) { xc_seq_write_begin(&s64.s); for (int k = 0; k < 16; k++) s64.v[k] = i; xc_seq_write_end(&s64.s); }
        else { uint32_t v, a[16]; do { v = xc_seq_read_begin(&s64.s); for (int k = 0; k < 16; k++) a[k] = s64.v[k]; } while (xc_seq_read_retry(&s64.s, v)); x ^= a[0] ^ a[15]; }
    } else if (p == P_SPIN16) {
        xc_spin_lock(&lk);
        if (c == 0) for (int k = 0; k < 4; k++) locked16.v[k] = i; else x ^= locked16.v[0] ^ locked16.v[3];
        xc_spin_unlock(&lk);
    }
    return x;
}

static void run_couple(int c, uint32_t p, stat_t *s)
{
    uint32_t x = c + 1;
    st_reset(s);
    for (uint32_t i = 1; i <= ITERS; i++) {
        uint32_t t = ccount();
        x = couple_iter(c, p, i, x);
        st_add(s, ccount() - t);
    }
    sink = x;
}

/* ---- critical section: spinlock vs Remote Core Locking (RCL) ---------- */
static struct { uint32_t a, b, c, d, n; } crit;  /* the protected state */
static inline void crit_op(uint32_t v) { crit.a += v; crit.b ^= v; crit.c += crit.a; crit.d = v; crit.n++; }

/* RCL: core 1 is the server; core 0 posts a request and waits for completion */
static struct { volatile uint32_t req, arg, ack; } slot;

enum { P_SPIN_UNC = 10, P_SPIN_CON, P_RCL_UNC, P_RCL_CON };
#define CRIT_N 20000

/* ---- SPSC FIFO ------------------------------------------------------------ */
#define FSIZE 256
static struct {
    volatile uint32_t head; uint32_t pad0[7];   /* producer-owned */
    volatile uint32_t tail; uint32_t pad1[7];   /* consumer-owned */
    uint32_t buf[FSIZE];
} fifo, fifo_back;

static inline bool f_push(typeof(fifo) *f, uint32_t v)
{
    uint32_t h = f->head;
    if (h - f->tail == FSIZE) return false;
    f->buf[h & (FSIZE - 1)] = v;
    xc_barrier();
    f->head = h + 1;
    return true;
}
static inline bool f_pop(typeof(fifo) *f, uint32_t *v)
{
    uint32_t t = f->tail;
    if (t == f->head) return false;
    *v = f->buf[t & (FSIZE - 1)];
    xc_barrier();
    f->tail = t + 1;
    return true;
}
enum { P_FIFO = 20, P_FIFO_BATCH, P_FIFO_RT };
#define FIFO_N 200000
static volatile uint32_t fifo_errors, fifo_got;

/* ---- SRAM bank contention: both cores load in a tight loop ---------------- */
enum { P_BANK = 30 };
static volatile uint32_t bank_addr1;
static uint32_t bank_loop(uint32_t addr)
{
    volatile const uint32_t *p = (const uint32_t *)addr;
    uint32_t t = ccount(), x = 0;
    for (int i = 0; i < 4096; i++) x += p[i & 255];
    sink = x;
    return ccount() - t;
}

/* ---- core 1 ----------------------------------------------------------------- */
static void core1(void)
{
    uint32_t last_go = 0;
    for (;;) {
        while (go == last_go) ;
        last_go = go;
        uint32_t p = phase;
        if (p >= P_BASE && p <= P_SPIN16) run_couple(1, p, &st1);
        else if (p == P_SPIN_CON) {
            for (int i = 0; i < CRIT_N; i++) { xc_spin_lock(&lk); crit_op(i); xc_spin_unlock(&lk); }
        } else if (p == P_RCL_UNC || p == P_RCL_CON) {
            uint32_t seen = slot.req, own = 0;
            while (phase == p) {
                uint32_t r = slot.req;
                if (r != seen) { seen = r; crit_op(slot.arg); xc_barrier(); slot.ack = r; }
                else if (p == P_RCL_CON && own < CRIT_N) { crit_op(own); own++; }   /* server's own work, no lock */
            }
        } else if (p == P_FIFO || p == P_FIFO_BATCH) {
            uint32_t expect = 0, v, err = 0;
            while (expect < FIFO_N) if (f_pop(&fifo, &v)) { err += v != expect; expect++; }
            fifo_errors = err; fifo_got = expect;
        } else if (p == P_FIFO_RT) {
            uint32_t v;
            for (int n = 0; n < 10000; ) if (f_pop(&fifo, &v)) { while (!f_push(&fifo_back, v)) ; n++; }
        } else if (p == P_BANK) {
            st_reset(&st1);
            for (int k = 0; k < 50; k++) st_add(&st1, bank_loop(bank_addr1));
        }
        done1 = last_go;
    }
}

static void start(uint32_t p) { phase = p; done1 = 0; go = go + 1; }
static void wait1(void) { while (done1 != go) ; }

int main(void)
{
    printf("\n=== xcore_bench2: cross-core design options (cycles, 240 = 1 us) ===\n");
    cpu1_start(core1);
    delay_us(10000);
    stat_t s0;

    /* 1. loose coupling: cost added to each loop iteration */
    printf("\n[1] loose coupling: loop iteration = fixed work + sharing; both cores running\n");
    static const char *const names[] = { "", "baseline (no sharing)", "plain 32-bit word", "seq counter, 16 B struct",
                                         "seq counter, 64 B struct", "spinlock, 16 B struct" };
    uint32_t base0 = 0, base1 = 0;
    for (uint32_t p = P_BASE; p <= P_SPIN16; p++) {
        start(p); run_couple(0, p, &s0); wait1();
        if (p == P_BASE) { base0 = st_avg(&s0); base1 = st_avg(&st1); }
        printf("  %-26s core0 (writer) avg %4lu max %4lu (+%3ld) | core1 (reader) avg %4lu max %4lu (+%3ld)\n", names[p],
               st_avg(&s0), s0.max, (long)st_avg(&s0) - (long)base0, st_avg(&st1), st1.max, (long)st_avg(&st1) - (long)base1);
    }

    /* 2. critical section: spinlock vs RCL */
    printf("\n[2] critical section (5-field update), %u ops on core 0\n", CRIT_N);
    for (int v = 0; v < 4; v++) {
        uint32_t p = P_SPIN_UNC + v;
        crit = (typeof(crit)){ 0 };
        st_reset(&s0);
        if (p != P_SPIN_UNC) start(p);
        delay_us(50);
        uint32_t t_all = ccount();
        for (int i = 0; i < CRIT_N; i++) {
            uint32_t t = ccount();
            if (p == P_SPIN_UNC || p == P_SPIN_CON) { xc_spin_lock(&lk); crit_op(i); xc_spin_unlock(&lk); }
            else { slot.arg = i; xc_barrier(); uint32_t r = slot.req + 1; slot.req = r; while (slot.ack != r) ; }
            st_add(&s0, ccount() - t);
        }
        t_all = ccount() - t_all;
        if (p == P_RCL_UNC || p == P_RCL_CON) { delay_us(20000); phase = 0; }   /* let the server finish its own ops */
        if (p != P_SPIN_UNC) wait1();
        uint32_t expect = p == P_SPIN_UNC || p == P_RCL_UNC ? CRIT_N : 2 * CRIT_N;
        static const char *const cn[] = { "spinlock, uncontended", "spinlock, both cores", "RCL, server idle", "RCL, server also working" };
        printf("  %-26s per op avg %4lu min %4lu max %5lu | ops done %lu (expect %lu) %s\n", cn[v],
               st_avg(&s0), s0.min, s0.max, crit.n, expect, crit.n == expect ? "ok" : "LOST UPDATES");
        (void)t_all;
    }

    /* 3. SPSC FIFO */
    printf("\n[3] shared FIFO (SPSC ring, 256 words), %u items core 0 -> core 1\n", FIFO_N);
    fifo.head = fifo.tail = 0; start(P_FIFO);
    st_reset(&s0);
    uint32_t t = ccount();
    for (uint32_t i = 0; i < FIFO_N; i++) { uint32_t t1 = ccount(); while (!f_push(&fifo, i)) ; st_add(&s0, ccount() - t1); }
    wait1(); t = ccount() - t;
    printf("  one item at a time: push avg %lu max %lu cycles, %lu cycles/item end to end, %lu errors, %lu received\n",
           st_avg(&s0), s0.max, t / FIFO_N, fifo_errors, fifo_got);
    fifo.head = fifo.tail = 0; start(P_FIFO_BATCH);
    t = ccount();
    for (uint32_t i = 0; i < FIFO_N; i += 16) {                 /* fill 16, publish once */
        uint32_t h = fifo.head;
        while (h + 16 - fifo.tail > FSIZE) ;
        for (int k = 0; k < 16; k++) fifo.buf[(h + k) & (FSIZE - 1)] = i + k;
        xc_barrier(); fifo.head = h + 16;
    }
    wait1(); t = ccount() - t;
    printf("  batches of 16:      %lu cycles/item end to end, %lu errors, %lu received\n", t / FIFO_N, fifo_errors, fifo_got);
    fifo.head = fifo.tail = fifo_back.head = fifo_back.tail = 0; start(P_FIFO_RT);
    st_reset(&s0);
    for (uint32_t i = 0; i < 10000; i++) { uint32_t v; t = ccount(); f_push(&fifo, i); while (!f_pop(&fifo_back, &v)) ; st_add(&s0, ccount() - t); }
    wait1();
    printf("  round trip (push -> echo -> pop): min %lu avg %lu max %lu cycles\n", s0.min, st_avg(&s0), s0.max);

    /* 4. SRAM bank contention: core 0 loads from A, core 1 from A (same) or far away */
    printf("\n[4] SRAM loads, 4096 per run, both cores at once\n");
    uint32_t A = (uint32_t)&fifo.buf[0];
    stat_t alone; st_reset(&alone); for (int k = 0; k < 50; k++) st_add(&alone, bank_loop(A));
    printf("  core 0 alone:                      %lu cycles\n", st_avg(&alone));
    uint32_t far[] = { A, A ^ 0x8000, A ^ 0x10000 };
    static const char *const bn[] = { "same 32 KB block", "other block (+32 KB)", "other block (+64 KB)" };
    for (int k = 0; k < 3; k++) {
        bank_addr1 = far[k]; start(P_BANK);
        st_reset(&s0); for (int j = 0; j < 50; j++) st_add(&s0, bank_loop(A));
        wait1();
        printf("  core 1 reading %-21s core0 %lu, core1 %lu cycles (max %lu / %lu)\n", bn[k], st_avg(&s0), st_avg(&st1), s0.max, st1.max);
    }
    printf("=== done ===\n");
    for (;;) ;
}
