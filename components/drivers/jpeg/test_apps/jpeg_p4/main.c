#include "jpeg.h"
#include "console.h"

#define W 64
#define H 48
static uint8_t pic[W * H * 3] __attribute__((aligned(64))), out[8192] __attribute__((aligned(64)));

int main(void)
{
    int err = 0;
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) { uint8_t *p = &pic[(y * W + x) * 3]; p[0] = x * 4; p[1] = y * 5; p[2] = 128; }
    int n = jpeg_encode(pic, W, H, JPEG_IN_RGB888, JPEG_SUB_420, 80, out, sizeof out);
    printf("jpeg: %d bytes\n", n);
    err += n < 200 || out[0] != 0xFF || out[1] != 0xD8 || out[n - 2] != 0xFF || out[n - 1] != 0xD9;
    printf("jpeg test errors %d\n", err);
    return err != 0;
}
