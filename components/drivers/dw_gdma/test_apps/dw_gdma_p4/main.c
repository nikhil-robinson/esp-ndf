#include "dw_gdma.h"
#include "console.h"
#include <string.h>

static uint8_t a[2048] __attribute__((aligned(64))), b[2048] __attribute__((aligned(64)));

int main(void)
{
    int err = 0;
    for (int i = 0; i < 2048; i++) a[i] = (uint8_t)(i * 5 + 1);
    err += dwg_m2m(b, a, sizeof a) != 0;
    err += memcmp(a, b, sizeof a) != 0;
    printf("dw_gdma test errors %d\n", err);
    return err != 0;
}
