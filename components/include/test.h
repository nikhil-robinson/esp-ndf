/* Tiny on-target test harness: CHECK records failures, METRIC prints numbers. */
#pragma once
#include "ndf.h"
#include "console.h"
#include "cpu.h"
#include "sys.h"
#include "unit.h"
#include "gpio.h"

extern int test_fail;
#define CHECK(cond) do { if (!(cond)) { test_fail++; unit_fail(__FILE__, __LINE__, #cond); } } while (0)   /* goes on */
#define METRIC(name, fmt, ...) printf("    %-34s " fmt "\n", name, __VA_ARGS__)

/* wait up to ms for cond; records a failure instead of hanging */
#define WAIT_FOR(cond, ms) ({ uint32_t _t = ccount(); bool _ok;                  \
    while (!(_ok = (cond)) && ccount() - _t < (ms) * 240000u) {}                    \
    if (!_ok) { test_fail++; unit_fail(__FILE__, __LINE__, "timeout: " #cond); } _ok; })

/* cycles taken by `stmt`, best of 8 runs (removes one-off effects) */
#define CYCLES(stmt) ({ uint32_t _b = ~0u; for (int _i = 0; _i < 8; _i++) { \
    uint32_t _t = ccount(); stmt; uint32_t _d = ccount() - _t; if (_d < _b) _b = _d; } _b; })

/* register a test function: NDF_UNIT(ord, name) after "void test_<name>(void) {...}"; apps run them sorted by ord
 * (later tests of one application may use state the earlier ones left) */
#define NDF_UNIT(ord, n) static const unit_t __pt_##n __attribute__((used, aligned(4), section(".ndf_reg.test.p" #ord "_" #n))) = \
    { #n, test_##n, __FILE__, __LINE__ }

/* Loopback detection: a test that needs a jumper between two pads checks for it and skips (passes with a note) when the
 * board has none, so one test list runs on boards wired differently (docs/WIRING.md lists the jumpers). */
static inline bool test_jumper(int a, int b)
{
    gpio_config(b, GPIO_INPUT | GPIO_PULLDOWN); gpio_config(a, GPIO_OUTPUT);
    gpio_set(a); delay_us(20); int hi = gpio_read(b);
    gpio_clr(a); delay_us(20); int lo = gpio_read(b);
    gpio_config(a, GPIO_INPUT); gpio_config(b, GPIO_INPUT);
    return hi && !lo;
}
#define NEED_JUMPER(a, b) do { if (!test_jumper((a), (b))) { printf("    SKIP: no jumper GPIO%d <-> GPIO%d on this board\n", (a), (b)); return; } } while (0)
/* The far end of a jumper A -> B, or A itself when the board has none (a pad reads back what it drives). */
static inline int test_far(int a, int b) { return test_jumper(a, b) ? b : a; }
