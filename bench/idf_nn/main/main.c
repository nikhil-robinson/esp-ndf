/* IDF sibling of components/lib/test_apps/nn_lib: ESP-NN 1.4.1 (S3 optimised, single core) on the shapes of bench/nn/layers.h:
 * cycles per layer (best of 3) and a weighted output checksum for the bit-exactness check against ours. */
#include <stdio.h>
#include <string.h>
#include "esp_cpu.h"
#include "esp_nn.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "layers.h"
#define A16 __attribute__((aligned(16)))
static int8_t in[32 * 32 * 32] A16, in2[4096] A16, out[32 * 32 * 32] A16, w[32 * 3 * 3 * 32] A16, fcw[512 * 64] A16;
static int32_t bias[64], mult[64], shift[64];
#define BEST(label, call, outp, n) do { uint32_t b = ~0u; for (int r = 0; r < 3; r++) { uint32_t t = esp_cpu_get_cycle_count(); call; t = esp_cpu_get_cycle_count() - t; if (t < b) b = t; } \
    printf("idf %-26s %9lu cycles  sum %ld\n", label, (unsigned long)b, (long)nn_sum(outp, n)); } while (0)

void app_main(void)
{
    vTaskDelay(100);
    void *scratch = heap_caps_aligned_alloc(16, 64 * 1024, MALLOC_CAP_INTERNAL);
    if (!scratch) { printf("idf_nn: no scratch memory\nidf_nn done\n"); for (;;) vTaskDelay(1000); }   /* never crash-loop (USB) */
    nn_fill_bias(bias, 64, 3); nn_fill_q(mult, shift, 64, -8);
    quant_data_t q = { shift, mult };
    { /* conv 1x1: 32x32x16 -> 32 */
      data_dims_t id = { 32, 32, 16, 1 }, fd = { 1, 1, 16, 32 }, od = { 32, 32, 32, 1 };
      conv_params_t cp = { NN_IN_OFF, NN_OUT_OFF, { 1, 1 }, { 0, 0 }, { 1, 1 }, { -128, 127 } };
      nn_fill8(in, 32 * 32 * 16, 5); nn_fill8(w, 32 * 16, 7);
      esp_nn_set_conv_scratch_buf(scratch); (void)esp_nn_get_conv_scratch_size(&id, &fd, &od, &cp);
      BEST("conv1x1 32x32x16->32", esp_nn_conv_s8(&id, in, &fd, w, bias, &od, out, &cp, &q), out, 32 * 32 * 32); }
    { /* conv 3x3 s1 p1: 16x16x16 -> 32 */
      data_dims_t id = { 16, 16, 16, 1 }, fd = { 3, 3, 16, 32 }, od = { 16, 16, 32, 1 };
      conv_params_t cp = { NN_IN_OFF, NN_OUT_OFF, { 1, 1 }, { 1, 1 }, { 1, 1 }, { -128, 127 } };
      nn_fill8(in, 16 * 16 * 16, 9); nn_fill8(w, 32 * 9 * 16, 11);
      printf("conv3x3 scratch %d\n", esp_nn_get_conv_scratch_size(&id, &fd, &od, &cp)); esp_nn_set_conv_scratch_buf(scratch);
      BEST("conv3x3 16x16x16->32 p1", esp_nn_conv_s8(&id, in, &fd, w, bias, &od, out, &cp, &q), out, 16 * 16 * 32); }
    for (int s = 1; s <= 2; s++) { /* depthwise 3x3 p1 mult 1: 32x32x32 */
      int ow = s == 1 ? 32 : 16;
      data_dims_t id = { 32, 32, 32, 1 }, fd = { 3, 3, 32, 1 }, od = { ow, ow, 32, 1 };
      dw_conv_params_t cp = { NN_IN_OFF, NN_OUT_OFF, 1, { s, s }, { 1, 1 }, { 1, 1 }, { -128, 127 } };
      nn_fill8(in, 32 * 32 * 32, 13); nn_fill8(w, 9 * 32, 15);
      printf("dw scratch %d\n", esp_nn_get_depthwise_conv_scratch_size(&id, &fd, &od, &cp)); esp_nn_set_depthwise_conv_scratch_buf(scratch);
      BEST(s == 1 ? "dwconv3x3 32x32x32 s1" : "dwconv3x3 32x32x32 s2", esp_nn_depthwise_conv_s8(&id, in, &fd, w, bias, &od, out, &cp, &q), out, ow * ow * 32); }
    nn_fill8(in, 512, 17); nn_fill8(fcw, 512 * 64, 19);
    BEST("fc 512->64", esp_nn_fully_connected_s8(in, NN_IN_OFF, 512, fcw, 0, bias, out, 64, NN_OUT_OFF, -9, 1518500250, -128, 127), out, 64);
    nn_fill8(in, 32 * 32 * 16, 21);
    BEST("maxpool 2x2 32x32x16", esp_nn_max_pool_s8(in, 32, 32, out, 16, 16, 2, 2, 2, 2, 0, 0, -128, 127, 16), out, 16 * 16 * 16);
    BEST("avgpool 2x2 32x32x16", esp_nn_avg_pool_s8(in, 32, 32, out, 16, 16, 2, 2, 2, 2, 0, 0, -128, 127, 16), out, 16 * 16 * 16);
    nn_fill8(in, 4096, 23); nn_fill8(in2, 4096, 25);
    BEST("add 4096", esp_nn_add_elementwise_s8(in, in2, 7, -3, 1073741824, 1288490189, -1, -2, 20, out, NN_OUT_OFF, 1932735283, -20, -128, 127, 4096), out, 4096);
    memcpy(out, in, 4096);
    BEST("relu6 4096", (memcpy(out, in, 4096), esp_nn_relu6_s8(out, 4096)), out, 4096);
    esp_nn_set_softmax_scratch_buf(scratch);
    nn_fill8(in, 1000, 27);
    BEST("softmax 100x10", esp_nn_softmax_s8(in, 100, 10, 1342177280, 23, -248, out), out, 1000);
    { /* model buffers from the heap (checked): the statics above already use most of DRAM */
      int8_t *bufA = in, *bufB = out;                           /* reuse the 32 KB layer buffers */
      int8_t *w1 = heap_caps_aligned_alloc(16, M_W1, MALLOC_CAP_INTERNAL), *w2 = heap_caps_aligned_alloc(16, M_W2 + 16, MALLOC_CAP_INTERNAL);
      int8_t *w3 = heap_caps_aligned_alloc(16, M_W3, MALLOC_CAP_INTERNAL), *w4 = heap_caps_aligned_alloc(16, M_W4, MALLOC_CAP_INTERNAL);
      static int8_t res[10]; static int32_t b32[32], b10[10];
      if (!w1 || !w2 || !w3 || !w4) { printf("idf_nn: no memory for the model\nidf_nn done\n"); for (;;) vTaskDelay(1000); }
      nn_fill8(w1, M_W1, 41); nn_fill8(w2, M_W2, 43); nn_fill8(w3, M_W3, 45); nn_fill8(w4, M_W4, 47); nn_fill_bias(b32, 32, 49); nn_fill_bias(b10, 10, 51);
      uint32_t best = ~0u;
      for (int r = 0; r < 3; r++) {
        nn_fill8(bufA, 32 * 32 * 16, 53);
        uint32_t t = esp_cpu_get_cycle_count();
        data_dims_t d16 = { 32, 32, 16, 1 }, d32 = { 32, 32, 32, 1 }, f3 = { 3, 3, 16, 32 }, fdw = { 3, 3, 32, 1 }, f1 = { 1, 1, 32, 32 };
        conv_params_t c3 = { NN_IN_OFF, NN_OUT_OFF, { 1, 1 }, { 1, 1 }, { 1, 1 }, { -128, 127 } }, c1 = { NN_IN_OFF, NN_OUT_OFF, { 1, 1 }, { 0, 0 }, { 1, 1 }, { -128, 127 } };
        dw_conv_params_t dwp = { NN_IN_OFF, NN_OUT_OFF, 1, { 1, 1 }, { 1, 1 }, { 1, 1 }, { -128, 127 } };
        esp_nn_set_conv_scratch_buf(scratch);
        esp_nn_conv_s8(&d16, bufA, &f3, w1, b32, &d32, bufB, &c3, &q);
        esp_nn_relu6_s8(bufB, 32 * 32 * 32);
        esp_nn_set_depthwise_conv_scratch_buf(scratch);
        esp_nn_depthwise_conv_s8(&d32, bufB, &fdw, w2, b32, &d32, bufA, &dwp, &q);
        esp_nn_set_conv_scratch_buf(scratch);
        esp_nn_conv_s8(&d32, bufA, &f1, w3, b32, &d32, bufB, &c1, &q);
        esp_nn_avg_pool_s8(bufB, 32, 32, bufA, 16, 16, 2, 2, 2, 2, 0, 0, -128, 127, 32);
        esp_nn_fully_connected_s8(bufA, NN_IN_OFF, 16 * 16 * 32, w4, 0, b10, bufB, 10, NN_OUT_OFF, -12, 1518500250, -128, 127);
        esp_nn_set_softmax_scratch_buf(scratch);
        esp_nn_softmax_s8(bufB, 1, 10, 1342177280, 23, -248, res);
        t = esp_cpu_get_cycle_count() - t; if (t < best) best = t;
      }
      printf("idf %-26s %9lu cycles  sum %ld\n", "model end-to-end", (unsigned long)best, (long)nn_sum(res, 10)); }
    printf("idf_nn done\n");
    for (;;) vTaskDelay(1000);
}
