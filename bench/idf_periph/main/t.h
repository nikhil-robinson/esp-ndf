/* Same metrics as examples/periph_test, measured through ESP-IDF APIs. */
#pragma once
#include <stdio.h>
#include <stdint.h>
#include "esp_attr.h"
#include <string.h>
#include <stdlib.h>
#include "esp_rom_sys.h"
static inline uint32_t ccount(void) { uint32_t c; __asm__ volatile("rsr.ccount %0" : "=r"(c)); return c; }
#define METRIC(name, fmt, ...) printf("    %-34s " fmt "\n", name, __VA_ARGS__)
#define CYCLES(stmt) ({ uint32_t _b = ~0u; for (int _i = 0; _i < 8; _i++) { \
    uint32_t _t = ccount(); stmt; uint32_t _d = ccount() - _t; if (_d < _b) _b = _d; } _b; })
void t_gpio(void);
void t_gptimer(void);
void t_ledc(void);
void t_mcpwm(void);
void t_pcnt(void);
void t_rmt(void);
void t_sdm(void);
void t_uart(void);
void t_i2c(void);
void t_spi(void);
void t_gdma(void);
void t_i2s(void);
void t_adc(void);
void t_twai(void);
void t_touch(void);
void t_flash(void);
