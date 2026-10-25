/* Shared layer shapes + deterministic data for components/lib/test_apps/nn_lib and bench/idf_nn (same LCG, same quant params). */
#pragma once
#include <stdint.h>
static uint32_t nn_seed;
static int8_t nn_rnd8(void) { nn_seed = nn_seed * 1103515245u + 12345u; return (int8_t)(nn_seed >> 16); }
static void nn_fill8(int8_t *p, int n, uint32_t s) { nn_seed = s; for (int i = 0; i < n; i++) p[i] = nn_rnd8(); }
static void nn_fill_bias(int32_t *p, int n, uint32_t s) { nn_seed = s; for (int i = 0; i < n; i++) { nn_seed = nn_seed * 1103515245u + 12345u; p[i] = (int32_t)(nn_seed >> 20) - 2048; } }
static void nn_fill_q(int32_t *mult, int32_t *shift, int n, int sh)
{ for (int i = 0; i < n; i++) { mult[i] = 1073741824 + i * 9871234; shift[i] = sh - (i & 1); } }
static int32_t nn_sum(const int8_t *p, int n) { int32_t s = 0; for (int i = 0; i < n; i++) s += p[i] * (i % 7 + 1); return s; }
#define NN_IN_OFF   17
#define NN_OUT_OFF  (-5)

/* tiny model for the end-to-end check (both sides): 32x32x16 input
 *   conv3x3 p1 16->32, relu6 (in place), dw3x3 p1 32, conv1x1 32->32, avgpool 2x2 -> 16x16x32, fc 8192->10, softmax
 * activations ping-pong between two internal-RAM buffers of 32 KB (the largest tensor: 32x32x32) */
#define M_W1 (32 * 9 * 16)
#define M_W2 (9 * 32)
#define M_W3 (32 * 32)
#define M_W4 (10 * 16 * 16 * 32)
