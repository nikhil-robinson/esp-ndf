/* Motor-control PWM: 2 units x (3 timers, 3 operators with A/B outputs,
 * dead-time, 3 capture channels). Timer clock 160 MHz / prescale.
 * Compare/period updates are shadowed and take effect at counter zero, so a
 * write never produces a runt pulse. */
#pragma once
#include "ndf.h"
#include "irq.h"

typedef enum { MCPWM_UP = 1, MCPWM_UPDOWN = 3 } mcpwm_mode_t;   /* edge / center aligned */
typedef enum { MCPWM_POS = 1, MCPWM_NEG = 2, MCPWM_BOTH = 3 } mcpwm_edge_t;

/* Timer t of unit u at freq_hz. Picks the finest tick that keeps the period
 * in 16 bits. Returns period in ticks (compare range 0..period), 0 if
 * impossible. *tick_hz receives the tick rate (may be NULL). */
uint32_t mcpwm_timer(int u, int t, uint32_t freq_hz, mcpwm_mode_t mode, uint32_t *tick_hz);
void     mcpwm_start(int u, int t);
void     mcpwm_stop(int u, int t);
/* Lock timer t to timer `master`'s zero with a phase offset in ticks (3-phase). */
void     mcpwm_sync(int u, int t, int master, uint32_t phase);
/* Call fn at every period start (counter zero) of timer t. fn NULL = off. */
int      mcpwm_on_period(int u, int t, irq_fn_t fn, void *arg, int level);

/* Operator op drives pins from timer t. Output is high while count < compare
 * (duty = cmp / period). pin_b < 0: A only. Independent A/B compares. */
int  mcpwm_pwm(int u, int op, int t, int pin_a, int pin_b);
/* Complementary pair from compare A with dead time (ticks): hi = A delayed on
 * rising, lo = !A delayed on rising of !A. Never both high. */
int  mcpwm_complementary(int u, int op, int t, int pin_hi, int pin_lo, uint32_t dead_rise, uint32_t dead_fall);

static inline void mcpwm_cmp(int u, int op, int ab, uint32_t ticks)
{
    *(volatile uint32_t *)((u ? 0x6002C000 : 0x6001E000) + 0x40 + 0x38 * op + 4 * ab) = ticks;
}
/* Brake: force both pins of op (after dead-time): 0 low, 1 high, -1 release.
 * Takes effect immediately, latched until released. */
void mcpwm_force(int u, int op, int level);

/* Capture channel ch (0..2) on pin: fn(arg) gets the 80 MHz capture time via
 * mcpwm_capture_value(); edge reports which edge (1 = pos, 2 = neg). */
typedef void (*mcpwm_cap_fn_t)(int ch, uint32_t ticks, int edge, void *arg);
int  mcpwm_capture(int u, int ch, int pin, mcpwm_edge_t edges, mcpwm_cap_fn_t fn, void *arg, int level);
#define MCPWM_CAP_HZ 80000000u

/* Carrier (chopper) on op's outputs: f = 160 MHz / 8 / n (n 1..16: 20 MHz..1.25 MHz), duty n/8 (0..6),
 * first pulse of each PWM high n carrier periods (1..16). hz = 0 off. Returns the carrier Hz. */
uint32_t mcpwm_carrier(int u, int op, uint32_t hz, int duty_eighths, int first_pulses);
/* Fault input f (0..2) of unit u on pin. */
int      mcpwm_fault_pin(int u, int f, int pin, bool active_high);
/* What op does on fault f: one_shot = latch until mcpwm_force(u, op, -1), else cycle by cycle (released
 * at the next period start once the fault is gone). level: 0 drive both outputs low, 1 high. */
int      mcpwm_fault_action(int u, int op, int f, bool one_shot, int level);
bool     mcpwm_fault_active(int u, int f);
/* Capture only every n-th selected edge: n = 1 or even up to 510 (hardware: odd n rounds up). Returns n used. */
uint32_t mcpwm_capture_prescale(int u, int ch, uint32_t n);
