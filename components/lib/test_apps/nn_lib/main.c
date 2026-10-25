/* int8 NN layers vs ESP-NN (sibling bench/idf_nn, same shapes/data from bench/nn/layers.h): cycles per layer (best
 * of 3) and the weighted output checksum, which must equal ESP-NN's (bit-exact). */
#include "console.h"
#include "nn.h"
#include "sys.h"
#include "xtensa.h"
#include <string.h>
#include "nn/layers.h"
#define A16 __attribute__((aligned(16)))
static int8_t in[32 * 32 * 32] A16, in2[4096] A16, out[32 * 32 * 32] A16, w[32 * 3 * 3 * 32] A16, fcw[512 * 64] A16;
static int32_t bias[64], mult[64], shift[64];
#define BEST(label, call, outp, n) do { uint32_t b = ~0u; for (int r = 0; r < 3; r++) { uint32_t t = ccount(); call; t = ccount() - t; if (t < b) b = t; } \
    printf("ndf %-26s %9lu cycles  sum %ld\n", label, (unsigned long)b, (long)nn_sum(outp, n)); } while (0)

int main(void)
{
    delay_ms(200);
    nn_fill_bias(bias, 64, 3); nn_fill_q(mult, shift, 64, -8);
    { nn_dims_t id = { 32, 32, 16 }, od = { 32, 32, 32 }; nn_conv_t cp = { NN_IN_OFF, NN_OUT_OFF, 1, 1, 0, 0, -128, 127 };
      nn_fill8(in, 32 * 32 * 16, 5); nn_fill8(w, 32 * 16, 7);
      BEST("conv1x1 32x32x16->32", nn_conv_s8(&id, in, 1, 1, w, bias, &od, out, &cp, mult, shift), out, 32 * 32 * 32); }
    { nn_dims_t id = { 16, 16, 16 }, od = { 16, 16, 32 }; nn_conv_t cp = { NN_IN_OFF, NN_OUT_OFF, 1, 1, 1, 1, -128, 127 };
      nn_fill8(in, 16 * 16 * 16, 9); nn_fill8(w, 32 * 9 * 16, 11);
      BEST("conv3x3 16x16x16->32 p1", nn_conv_s8(&id, in, 3, 3, w, bias, &od, out, &cp, mult, shift), out, 16 * 16 * 32); }
    for (int s = 1; s <= 2; s++) {
      int ow = s == 1 ? 32 : 16;
      nn_dims_t id = { 32, 32, 32 }, od = { ow, ow, 32 }; nn_conv_t cp = { NN_IN_OFF, NN_OUT_OFF, s, s, 1, 1, -128, 127 };
      nn_fill8(in, 32 * 32 * 32, 13); nn_fill8(w, 9 * 32, 15);
      BEST(s == 1 ? "dwconv3x3 32x32x32 s1" : "dwconv3x3 32x32x32 s2", nn_dwconv_s8(&id, in, 3, 3, w, bias, &od, out, &cp, mult, shift), out, ow * ow * 32); }
    nn_fill8(in, 512, 17); nn_fill8(fcw, 512 * 64, 19);
    BEST("fc 512->64", nn_fc_s8(in, NN_IN_OFF, 512, fcw, 0, bias, out, 64, NN_OUT_OFF, -9, 1518500250, -128, 127), out, 64);
    nn_fill8(in, 32 * 32 * 16, 21);
    BEST("maxpool 2x2 32x32x16", nn_maxpool_s8(in, 32, 32, out, 16, 16, 2, 2, 2, 2, 0, 0, -128, 127, 16), out, 16 * 16 * 16);
    BEST("avgpool 2x2 32x32x16", nn_avgpool_s8(in, 32, 32, out, 16, 16, 2, 2, 2, 2, 0, 0, -128, 127, 16), out, 16 * 16 * 16);
    nn_fill8(in, 4096, 23); nn_fill8(in2, 4096, 25);
    BEST("add 4096", nn_add_s8(in, in2, 7, -3, 1073741824, 1288490189, -1, -2, 20, out, NN_OUT_OFF, 1932735283, -20, -128, 127, 4096), out, 4096);
    BEST("relu6 4096", (memcpy(out, in, 4096), nn_relu6_s8(out, 4096)), out, 4096);
    nn_fill8(in, 1000, 27);
    BEST("softmax 100x10", nn_softmax_s8(in, 100, 10, 1342177280, 23, -248, out), out, 1000);
    { /* end to end: bench/nn/layers.h tiny model */
      int8_t *bufA = in, *bufB = out; static int8_t w1[M_W1] A16, w2[M_W2] A16, w3[M_W3] A16, w4[M_W4] A16, res[10];   /* reuse the layer buffers */
      static int32_t b32[32], b10[10];
      nn_fill8(w1, M_W1, 41); nn_fill8(w2, M_W2, 43); nn_fill8(w3, M_W3, 45); nn_fill8(w4, M_W4, 47); nn_fill_bias(b32, 32, 49); nn_fill_bias(b10, 10, 51);
      uint32_t best = ~0u;
      for (int r = 0; r < 3; r++) {
        nn_fill8(bufA, 32 * 32 * 16, 53);
        uint32_t t = ccount();
        nn_dims_t d16 = { 32, 32, 16 }, d32 = { 32, 32, 32 };
        nn_conv_t c3 = { NN_IN_OFF, NN_OUT_OFF, 1, 1, 1, 1, -128, 127 }, c1 = { NN_IN_OFF, NN_OUT_OFF, 1, 1, 0, 0, -128, 127 };
        nn_conv_s8(&d16, bufA, 3, 3, w1, b32, &d32, bufB, &c3, mult, shift);
        nn_relu6_s8(bufB, 32 * 32 * 32);
        nn_dwconv_s8(&d32, bufB, 3, 3, w2, b32, &d32, bufA, &c3, mult, shift);
        nn_conv_s8(&d32, bufA, 1, 1, w3, b32, &d32, bufB, &c1, mult, shift);
        nn_avgpool_s8(bufB, 32, 32, bufA, 16, 16, 2, 2, 2, 2, 0, 0, -128, 127, 32);
        nn_fc_s8(bufA, NN_IN_OFF, 16 * 16 * 32, w4, 0, b10, bufB, 10, NN_OUT_OFF, -12, 1518500250, -128, 127);
        nn_softmax_s8(bufB, 1, 10, 1342177280, 23, -248, res);
        t = ccount() - t; if (t < best) best = t;
      }
      printf("ndf %-26s %9lu cycles  sum %ld\n", "model end-to-end", (unsigned long)best, (long)nn_sum(res, 10)); }
    printf("nn_lib done\n");
    for (;;) {}
}
