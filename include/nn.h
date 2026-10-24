/* int8 neural-network layers (ESP-NN / TFLite Micro int8 semantics, bit-exact with ESP-NN's reference kernels):
 * NHWC activations, conv filters [out_ch][fh][fw][in_ch], depthwise filters [fh][fw][ch], per-channel or per-tensor
 * requantisation (mult Q31, shift: >0 left, <0 right). PIE int8 dot products where the shapes allow. */
#pragma once
#include "ndf.h"

typedef struct { int32_t w, h, c; } nn_dims_t;
typedef struct { int32_t in_off, out_off, stride_w, stride_h, pad_w, pad_h, act_min, act_max; } nn_conv_t;

/* TFLite requantisation: round(x * mult / 2^31) with saturation, then a rounding shift (bit-exact) */
int32_t nn_requant(int32_t x, int32_t mult, int32_t shift);

void nn_conv_s8(const nn_dims_t *in_d, const int8_t *in, int fh, int fw, const int8_t *filt, const int32_t *bias,
                const nn_dims_t *out_d, int8_t *out, const nn_conv_t *p, const int32_t *mult, const int32_t *shift);
void nn_dwconv_s8(const nn_dims_t *in_d, const int8_t *in, int fh, int fw, const int8_t *filt, const int32_t *bias,
                  const nn_dims_t *out_d, int8_t *out, const nn_conv_t *p, const int32_t *mult, const int32_t *shift);
void nn_fc_s8(const int8_t *in, int32_t in_off, int len, const int8_t *w, int32_t w_off, const int32_t *bias, int8_t *out,
              int out_n, int32_t out_off, int32_t shift, int32_t mult, int32_t act_min, int32_t act_max);
void nn_maxpool_s8(const int8_t *in, int iw, int ih, int8_t *out, int ow, int oh, int sw, int sh, int fw, int fh,
                   int pw, int ph, int32_t act_min, int32_t act_max, int ch);
void nn_avgpool_s8(const int8_t *in, int iw, int ih, int8_t *out, int ow, int oh, int sw, int sh, int fw, int fh,
                   int pw, int ph, int32_t act_min, int32_t act_max, int ch);
void nn_add_s8(const int8_t *a, const int8_t *b, int32_t a_off, int32_t b_off, int32_t a_mult, int32_t b_mult,
               int32_t a_shift, int32_t b_shift, int32_t left_shift, int8_t *out, int32_t out_off, int32_t out_mult,
               int32_t out_shift, int32_t act_min, int32_t act_max, int n);
void nn_mul_s8(const int8_t *a, const int8_t *b, int32_t a_off, int32_t b_off, int8_t *out, int32_t out_off,
               int32_t out_mult, int32_t out_shift, int32_t act_min, int32_t act_max, int n);
void nn_relu6_s8(int8_t *x, int n);
void nn_softmax_s8(const int8_t *in, int rows, int cols, int32_t mult, int32_t shift, int32_t diff_min, int8_t *out);
