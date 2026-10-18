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

/* ---- continuous (DMA) mode, ADC1 only on the S3 ------------------------------
 * A pattern of up to 12 channels is converted round-robin at sample_hz
 * (611..83333 conversions/s in total); results stream by DMA into a ring of
 * `frames` buffers of frame_samples words each. Each word: */
#define ADC_CONT_DATA(w)  ((w) & 0xFFF)
#define ADC_CONT_CH(w)    (((w) >> 13) & 0xF)

typedef struct { uint8_t ch; adc_atten_t atten; } adc_cont_ch_t;

/* buf: frames * frame_samples words in internal RAM; frame_samples <= 1023 */
int  adc_cont_init(const adc_cont_ch_t *chs, int n, uint32_t sample_hz,
                   uint32_t *buf, int frame_samples, int frames);
/* called from the DMA interrupt for every finished frame (NULL: polling only) */
int  adc_cont_on_frame(void (*fn)(const uint32_t *frame, int samples, void *arg), void *arg, int level);
int  adc_cont_start(void);
void adc_cont_stop(void);
/* polling: next finished frame or NULL; frames are recycled once read */
const uint32_t *adc_cont_get(int *samples);
uint32_t adc_cont_overruns(void);        /* frames the reader did not take in time */
uint32_t adc_cont_sync_errors(void);     /* must stay 0: frame interrupt vs descriptor bookkeeping */

/* internal, shared with the one-shot driver */
void adc_power_up(void);
void adc_load_code(int unit, int atten);
