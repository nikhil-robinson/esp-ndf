/* LED PWM controller: 4 timers, 8 channels, up to 14-bit duty, APB 80 MHz. */
#pragma once
#include "ndf.h"

#define LEDC_TIMERS   4
#define LEDC_CHANNELS 8

/* Configure timer for freq/resolution. Returns the achieved frequency in
 * milli-Hz (exact, from the real divider), 0 if impossible. */
uint64_t ledc_timer(int timer, uint32_t freq_hz, int res_bits);

/* Route channel to pin, attach to timer, set duty (0 .. 2^res). */
int  ledc_channel(int ch, int timer, int pin, uint32_t duty);
void ledc_stop(int ch, bool idle_level);

/* Duty update, takes effect at the next PWM period. hpoint = phase offset. */
void ledc_duty(int ch, uint32_t duty);
void ledc_duty_phase(int ch, uint32_t duty, uint32_t hpoint);

/* Hardware fade: step duty by +-1 every `cycles` periods, `steps` times. */
void ledc_fade(int ch, uint32_t from, bool up, uint32_t steps, uint32_t cycles);

/* Gamma fade (perceptually linear): from -> to duty over ms, duty = max * b^(gamma/100) along a linear
 * brightness b; 16 hardware segments chained from the fade-end interrupt. gamma_x100 = 220 for LEDs. */
int  ledc_fade_gamma(int ch, uint32_t from, uint32_t to, uint32_t ms, uint32_t gamma_x100);
bool ledc_fade_busy(int ch);
