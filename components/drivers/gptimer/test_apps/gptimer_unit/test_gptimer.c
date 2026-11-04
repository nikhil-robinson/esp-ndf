#include "test.h"
#include "gptimer.h"
#include "sys.h"

typedef struct { uint32_t n, last, jmax, lat_min, lat_max; uint32_t period; int id; bool lat; } st_t;
static st_t st;

RAM_FN static void alarm_cb(void *arg)
{
    st_t *s = arg;
    uint32_t now = ccount();
    if (s->lat) {                                /* counter restarted at alarm: value = latency */
        uint32_t c = (uint32_t)gpt_get(s->id);
        if (c < s->lat_min) s->lat_min = c;
        if (c > s->lat_max) s->lat_max = c;
    }
    if (s->n && s->last) {
        int32_t d = (int32_t)(now - s->last - s->period);
        uint32_t j = d < 0 ? -d : d;
        if (j > s->jmax) s->jmax = j;
    }
    s->last = now; s->n++;
}

static void periodic(int id, int level, uint32_t hz, bool lat, uint32_t count)
{
    st = (st_t){ .period = 240000000 / (40000000 / count), .id = id, .lat = lat, .lat_min = ~0u };
    CHECK(gpt_init(id, hz) == hz);
    CHECK(gpt_alarm(id, count, true, alarm_cb, &st, level) == 0);
    gpt_start(id);
    irq_enable_all();
    uint32_t t0 = ccount();
    while (READ_ONCE(st.n) < 5000 && ccount() - t0 < 240000000) {}   /* 1 s timeout */
    if (st.n < 5000) {
        uint32_t pend, ena;
        __asm__ volatile("rsr.interrupt %0; rsr.intenable %1" : "=r"(pend), "=r"(ena));
        printf("    timeout: n=%lu cfg=%08lx raw=%08lx pend=%08lx ena=%08lx cnt=%lu\n", st.n,
               REG(0x6001F000 + (id >> 1) * 0x1000 + 0x24 * (id & 1)),
               REG(0x6001F074 + (id >> 1) * 0x1000), pend, ena, (uint32_t)gpt_get(id));
    }
    gpt_stop(id);
    gpt_alarm_off(id);
}

void test_gptimer(void)
{
    CHECK(gpt_init(0, 3000000) == 0);           /* no exact divider -> refused */
    CHECK(gpt_init(0, 1000000) == 1000000);

    /* rate: 10 ms of CPU cycles must read 10000 +-1 ticks */
    gpt_start(0);
    uint64_t a = gpt_get(0); uint32_t t = ccount();
    while (ccount() - t < 2400000) {}
    uint64_t b = gpt_get(0);
    gpt_stop(0);
    int32_t err = (int32_t)(b - a) - 10000;
    CHECK(err >= -1 && err <= 1);
    METRIC("1 MHz over 10 ms (expect 10000)", "%lu ticks", (uint32_t)(b - a));

    gpt_set(1, 0x123456789ull); gpt_init(1, 1000000); gpt_set(1, 0x123456789ull);
    CHECK(gpt_get(1) == 0x123456789ull);         /* 54-bit load/readback */
    METRIC("gpt_get()", "%lu cycles", CYCLES(gpt_get(0)));

    periodic(2, 1, 40000000, true, 4000);        /* 10 kHz, latency in 25 ns ticks */
    CHECK(st.n >= 5000);
    METRIC("alarm -> callback (L1)", "min %lu max %lu ns", st.lat_min * 25, st.lat_max * 25);
    METRIC("10 kHz period jitter (L1)", "max %lu cycles", st.jmax);

    periodic(3, 5, 40000000, false, 4000);
    CHECK(st.n >= 5000);
    METRIC("10 kHz period jitter (L5 fast)", "max %lu cycles", st.jmax);

    /* one-shot fires exactly once */
    st = (st_t){ 0 };
    gpt_init(0, 1000000);
    gpt_alarm(0, 100, false, alarm_cb, &st, 1);
    gpt_start(0);
    delay_us(1000);
    gpt_stop(0); gpt_alarm_off(0);
    CHECK(st.n == 1);
}

NDF_UNIT(010, gptimer);
