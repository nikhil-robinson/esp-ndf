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
void test_clkout(void);
void test_gptimer(void);
void test_ledc(void);
void test_ledc_gamma(void);
void test_mcpwm(void);
void test_mcpwm_ext(void);
void test_pcnt(void);
void test_pcnt_ext(void);
void test_rmt(void);
void test_rmt_ext(void);
void test_sdm(void);
void test_uart(void);
void test_uart_modes(void);
void test_i2c(void);
void test_i2c_ext(void);
void test_gdma(void);
void test_spi(void);
void test_spi_hd(void);
void test_i2s(void);
void test_i2s_modes(void);
void test_lcd_i80(void);
void test_cam(void);
void test_adc(void);
void test_twai(void);
void test_twai_alerts(void);
void test_touch(void);
void test_touch_ext(void);
void test_flash(void);
void test_partition(void);
void test_nvs(void);
void test_fat(void);
void test_fat_ext(void);
void test_adc_cont(void);
void test_adc_filter(void);
void test_rtcio(void);
void test_nvs_enc(void);
void test_spiffs(void);
void test_spiffs_rename(void);
void test_pfs(void);
