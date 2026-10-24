/* shared by examples/fastmath_test and bench/idf_math: 1024 arguments per function, cycles per call */
#define FM_N 1024
static float fm_x[FM_N], fm_y[FM_N];
static volatile float fm_o[FM_N];                                  /* volatile: the timed calls cannot be folded away */
static void fm_args(void) { for (int i = 0; i < FM_N; i++) { fm_x[i] = -10.0f + 20.0f * i / FM_N; fm_y[i] = 3.0f - 6.0f * ((i * 37) % FM_N) / FM_N; } }
