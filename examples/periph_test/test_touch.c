#include "test.h"
#include "touch.h"
#include "gpio.h"
#include "sys.h"

/* pad 10 = GPIO10 carries the jumper wire + GPIO11's pad: much more capacitance
 * than the bare pads 1 and 14 (same ordering ESP-IDF reports on this board). */
static uint32_t avg(int pad) { uint32_t s = 0; for (int i = 0; i < 16; i++) { s += touch_read(pad); delay_us(2000); } return s / 16; }

void test_touch(void)
{
    gpio_config(11, GPIO_INPUT);                           /* floating */
    CHECK(touch_init(BIT(1) | BIT(10) | BIT(14)) == 0);
    delay_us(50000);
    uint32_t p1 = avg(1), p10 = avg(10), p14 = avg(14);
    printf("    raw: pad1 %lu, pad14 %lu, pad10 %lu (jumper)\n", p1, p14, p10);
    CHECK(p1 > 1000 && p14 > 1000 && p10 > 2 * p1 && p10 > 2 * p14);
    METRIC("touch_read()", "%lu cycles", CYCLES(touch_read(10)));
    gpio_config(11, GPIO_INPUT);
}

/* "Touch" = pad 10's charge slope 7 -> 6 (+16 % raw, like a finger), then back. */
static volatile int ev_on, ev_off, ev_pad;
static void on_touch(int pad, bool t, void *a) { (void)a; ev_pad = pad; if (t) ev_on++; else ev_off++; }

void test_touch_ext(void)
{
    __asm__ volatile("rsil a2, 0" ::: "a2");
    gpio_config(11, GPIO_INPUT);
    touch_init(BIT(10));
    touch_filter(true);
    delay_us(200000);
    uint32_t raw = avg(10), base = touch_benchmark(10), sm = touch_smooth(10);
    touch_threshold(10, base / 20);                          /* 5 % */
    ev_on = ev_off = 0;
    CHECK(touch_on_event(on_touch, 0) == 0);
    REG_FIELD(0x60008150, 29, 3, 6); delay_us(100000);      /* touch */
    int on = ev_on; uint16_t act = touch_active();
    REG_FIELD(0x60008150, 29, 3, 7); delay_us(100000);      /* release */
    int off = ev_off;
    printf("    filter: raw %lu, smooth %lu, baseline %lu; threshold %lu: touch -> %d event(s) pad %d (active %04x), release -> %d\n",
           raw, sm, base, base / 20, on, ev_pad, act, off);
    CHECK(base > raw - raw / 20 && base < raw + raw / 20 && on == 1 && (act & BIT(10)) && off == 1 && ev_pad == 10);
    touch_on_event(0, 0);
    const int8_t px[3] = { 10, 0, 0 };
    touch_proximity(px, 50);
    delay_us(300000);
    uint32_t pc = touch_proximity_count(0);
    printf("    proximity pad 10, 50 scans: count %lu\n", pc);
    CHECK(pc > 0 && pc <= 50);
    touch_proximity((const int8_t[3]){ 0, 0, 0 }, 0);
    touch_filter(false);
}

/* denoise: spread (max - min) of 200 raw reads on pad 10 without and with denoise (grade 4-bit, 5 pF) */
static uint32_t spread(void)
{
    uint32_t lo = ~0u, hi = 0;
    for (int i = 0; i < 200; i++) { uint32_t v = touch_read(10); if (v < lo) lo = v; if (v > hi) hi = v; delay_us(2000); }
    return hi - lo;
}

void test_touch_denoise(void)
{
    touch_init(BIT(10));
    delay_us(100000);
    uint32_t r0 = avg(10), s0 = spread();
    touch_denoise(3, 0);
    touch_init(BIT(10));
    delay_us(100000);
    uint32_t r1 = avg(10), s1 = spread(), dn = touch_denoise_data();
    touch_denoise(-1, 0);
    touch_init(BIT(10));
    printf("    pad 10 avg raw %lu spread %lu -> denoise on: raw %lu spread %lu, reference %lu\n", r0, s0, r1, s1, dn);
    CHECK(dn > 0 && s1 > 0 && r1 + 50 > r0 && r1 < r0 + 50);      /* reference live, scan not stalled; the <= 15-count subtraction is below the read noise */
}

