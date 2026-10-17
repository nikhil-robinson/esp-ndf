/* Pulse counter: 4 units, 2 channels each, hardware glitch filter.
 * Counts are extended to 32 bits in software without gaps: the hardware
 * 16-bit counter wraps at +-PCNT_WRAP and an IRQ accumulates. */
#pragma once
#include "ndf.h"

#define PCNT_UNITS 4
#define PCNT_WRAP  30000

typedef enum { PCNT_EDGE_OFF = 0, PCNT_EDGE_UP = 1, PCNT_EDGE_DOWN = 2 } pcnt_edge_t;
typedef enum { PCNT_CTRL_KEEP = 0, PCNT_CTRL_INVERT = 1, PCNT_CTRL_HOLD = 2 } pcnt_ctrl_t;

/* Generic channel: count edges of sig_pin, modified by the level of ctrl_pin
 * (ctrl_pin < 0: constant high). */
int  pcnt_channel(int unit, int ch, int sig_pin, int ctrl_pin,
                  pcnt_edge_t pos, pcnt_edge_t neg, pcnt_ctrl_t when_high, pcnt_ctrl_t when_low);

/* Quadrature encoder, x4 decoding. Positive count when A leads B. */
int  pcnt_quadrature(int unit, int pin_a, int pin_b);

/* Ignore pulses shorter than ns (0 = off). Max ~12.7 us (1023 APB cycles). */
void pcnt_filter(int unit, uint32_t ns);

int32_t pcnt_get(int unit);
void    pcnt_clear(int unit);
void    pcnt_pause(int unit, bool pause);
