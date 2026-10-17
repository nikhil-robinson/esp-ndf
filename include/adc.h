/* SAR ADC one-shot, 12-bit, both units, with the chip's eFuse calibration
 * (same curve-fitting math as ESP-IDF, integer-exact).
 * Unit 1: channel n = GPIO n+1 (1..10). Unit 2: channel n = GPIO n+11 (11..20). */
#pragma once
#include "ndf.h"

typedef enum { ADC_ATTEN_0DB = 0, ADC_ATTEN_2_5DB = 1, ADC_ATTEN_6DB = 2, ADC_ATTEN_12DB = 3 } adc_atten_t;

int  adc_channel(int unit, int ch, adc_atten_t atten);   /* puts the pad in analog mode */
int  adc_read_raw(int unit, int ch);                       /* 0..4095, -1 on error */
int  adc_raw_to_mv(int unit, adc_atten_t atten, int raw);  /* -1 if chip has no calibration */
int  adc_read_mv(int unit, int ch);
bool adc_calibrated(void);
