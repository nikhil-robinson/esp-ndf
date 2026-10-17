/* Shared control-loop benchmark: compiled unchanged into the bare-metal
 * image (bm/) and the ESP-IDF/FreeRTOS image (idf_bench/).
 * Each core runs it at the same time, so bus contention matches too.
 * Load: 20 kHz control loop on each core + on core0 a 10 kHz timer ISR
 * (TIMG0 T0) and 1 kHz USB telemetry.
 * The includer defines BENCH_FN (e.g. IRAM_ATTR) and provides
 * bench_usb_tx(buf, n) -> bytes accepted (must not block). */
#pragma once
#include <stdint.h>

#ifndef BENCH_FN
#define BENCH_FN
#endif

#define CPU_HZ   240000000u
#define LOOP_HZ  20000u                 /* 20 kHz control loop          */
#define PERIOD   (CPU_HZ / LOOP_HZ)     /* 12000 cycles = 50 us         */
#define ITERS    (LOOP_HZ * 5)          /* 5 s of control loop          */

#ifndef REG
#define REG(a)   (*(volatile uint32_t *)(a))
#endif
#ifndef GPIO_OUT_W1TS
#define GPIO_OUT_W1TS 0x60004008
#define GPIO_OUT_W1TC 0x6000400C
#endif
#define BENCH_PIN(core) ((core) ? 5 : 4) /* scope these: high = work()  */
#define ISR_PIN  6                      /* toggles every timer ISR      */
#define ISR_HZ   10000u
#define ISR_PERIOD (CPU_HZ / ISR_HZ)    /* 24000 cycles = 100 us        */
#define TLM_EVERY 20                    /* telemetry every 20 iters = 1 kHz */

typedef struct {
    uint32_t iters, misses, lat_max, exec_min, exec_max, work_per_s;
    uint64_t lat_sum, exec_sum;
    uint32_t usb_bytes;
} bench_stats_t;

typedef struct { uint32_t n, last, jit_max; uint64_t jit_sum; } isr_stats_t;
static isr_stats_t bench_isr_res;
static volatile int bench_isr_on;
static int bench_usb_tx(const char *buf, int n);

typedef struct { float kp, ki, kd, i, prev; } ctl_t;

static volatile uint32_t bench_ready[2];
static volatile float bench_in = 1.0f, bench_sink;
static bench_stats_t bench_res[2];

static inline __attribute__((always_inline)) uint32_t ccount(void)
{
    uint32_t c;
    __asm__ volatile("rsr.ccount %0" : "=r"(c));
    return c;
}

/* Fake control law: 8 PID axes, fixed op count, float like a real robot. */
static inline __attribute__((always_inline)) float work(ctl_t *c, float x)
{
    float out = 0;
    for (int a = 0; a < 8; a++) {
        float e = x - c[a].prev * 0.5f;
        c[a].i += e * 1e-4f;
        float d = e - c[a].prev;
        c[a].prev = e;
        out += c[a].kp * e + c[a].ki * c[a].i + c[a].kd * d;
    }
    return out;
}

/* Body of the timer ISR on both platforms: period jitter vs ideal 100 us. */
BENCH_FN static void bench_isr(void)
{
    uint32_t now = ccount();
    isr_stats_t *r = &bench_isr_res;
    REG(r->n & 1 ? GPIO_OUT_W1TC : GPIO_OUT_W1TS) = 1u << ISR_PIN;
    if (bench_isr_on && r->last) {
        int32_t d = (int32_t)(now - r->last - ISR_PERIOD);
        uint32_t j = d < 0 ? -d : d;
        r->jit_sum += j; if (j > r->jit_max) r->jit_max = j;
        r->n++;
    }
    r->last = now;
}

static char *hex8(char *p, uint32_t v)
{
    for (int k = 28; k >= 0; k -= 4) *p++ = "0123456789abcdef"[(v >> k) & 15];
    return p;
}

BENCH_FN static void bench_run(int core)
{
    bench_stats_t *s = &bench_res[core];
    const uint32_t pin = 1u << BENCH_PIN(core);
    ctl_t c[8];
    for (int a = 0; a < 8; a++) {
        c[a].kp = 1.2f; c[a].ki = 0.3f; c[a].kd = 0.05f; c[a].i = c[a].prev = 0;
    }

    /* barrier: both cores start together */
    bench_ready[core] = 1;
    while (!(bench_ready[0] && bench_ready[1])) {}

    /* 1) periodic loop: wake latency (start - deadline) and exec time */
    s->exec_min = ~0u;
    if (core == 0) bench_isr_on = 1;
    char tlm[40];
    uint32_t next = ccount() + PERIOD;
    for (uint32_t i = 0; i < ITERS; i++) {
        while ((int32_t)(ccount() - next) < 0) {}
        uint32_t t0 = ccount();
        REG(GPIO_OUT_W1TS) = pin;
        bench_sink = work(c, bench_in);
        REG(GPIO_OUT_W1TC) = pin;
        if (core == 0 && i % TLM_EVERY == 0) {       /* telemetry is part of the loop cost */
            char *p = tlm; *p++ = 'T'; *p++ = ' ';
            p = hex8(p, i); *p++ = ' '; p = hex8(p, s->exec_max); *p++ = ' ';
            p = hex8(p, s->lat_max); *p++ = '\n';
            s->usb_bytes += bench_usb_tx(tlm, p - tlm);
        }
        uint32_t ex = ccount() - t0, lat = t0 - next;
        s->lat_sum += lat;   if (lat > s->lat_max) s->lat_max = lat;
        s->exec_sum += ex;   if (ex > s->exec_max) s->exec_max = ex;
        if (ex < s->exec_min) s->exec_min = ex;
        next += PERIOD;
        if ((int32_t)(ccount() - next) >= 0) s->misses++;
    }
    s->iters = ITERS;
    if (core == 0) bench_isr_on = 0;

    /* 2) raw throughput: work() calls completed in 1 s */
    uint32_t n = 0, t0 = ccount();
    while (ccount() - t0 < CPU_HZ) { bench_sink = work(c, bench_in); n++; }
    s->work_per_s = n;
}

/* cycles -> ns at 240 MHz: c * 25 / 6 */
#define NS(c) ((uint32_t)((uint64_t)(c) * 25 / 6))

static void bench_print(int (*pf)(const char *, ...), const char *name)
{
    pf("\n=== %s  (%u Hz loop, %u iters/core) ===\n", name, LOOP_HZ, ITERS);
    for (int k = 0; k < 2; k++) {
        bench_stats_t *s = &bench_res[k];
        pf("core%d: wake lat avg %u ns max %u ns | exec min %u avg %u max %u ns | misses %u | work/s %u\n",
           k, NS(s->lat_sum / s->iters), NS(s->lat_max),
           NS(s->exec_min), NS(s->exec_sum / s->iters), NS(s->exec_max),
           s->misses, s->work_per_s);
    }
    isr_stats_t *r = &bench_isr_res;
    pf("isr  : %u calls, period jitter avg %u ns max %u ns | usb telemetry %u bytes\n",
       r->n, NS(r->n ? r->jit_sum / r->n : 0), NS(r->jit_max), bench_res[0].usb_bytes);
}
