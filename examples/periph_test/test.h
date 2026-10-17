/* Tiny on-target test harness: CHECK records failures, METRIC prints numbers. */
#pragma once
#include "ndf.h"
#include "console.h"
#include "xtensa.h"

extern int test_fail;
#define CHECK(cond) do { if (!(cond)) { test_fail++; \
    printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define METRIC(name, fmt, ...) printf("    %-34s " fmt "\n", name, __VA_ARGS__)

/* wait up to ms for cond; records a failure instead of hanging */
#define WAIT_FOR(cond, ms) ({ uint32_t _t = ccount(); bool _ok;                  \
    while (!(_ok = (cond)) && ccount() - _t < (ms) * 240000u) {}                    \
    if (!_ok) { test_fail++; printf("    TIMEOUT %s:%d: %s\n", __FILE__, __LINE__, #cond); } _ok; })

/* cycles taken by `stmt`, best of 8 runs (removes one-off effects) */
#define CYCLES(stmt) ({ uint32_t _b = ~0u; for (int _i = 0; _i < 8; _i++) { \
    uint32_t _t = ccount(); stmt; uint32_t _d = ccount() - _t; if (_d < _b) _b = _d; } _b; })

void test_gpio(void);
void test_gptimer(void);
void test_ledc(void);
void test_mcpwm(void);
void test_pcnt(void);
void test_rmt(void);
void test_sdm(void);
void test_uart(void);
